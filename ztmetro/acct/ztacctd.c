// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztacctd: battery and data use per app, for ztOS. A root daemon; ztacct (the
 * command) and the "battery + data" app read it through /run/ztacct/sock
 * (acct-common.h). See README.md beside this file for the design.
 *
 * What makes a process part of an app (classify()), first match wins:
 *
 *   kernel threads				"Kernel"
 *   inside Waydroid's container (lxc.payload.waydroid)
 *	an Android app (uid >= 10000)		"<its label>" by package (cmdline)
 *	anything else in there			"Android system"
 *   a cgroup of zrnperfd's tickle (zrn-tickle-PID) or a systemd app scope
 *   (app-ID.scope): the app that cgroup was made for
 *   system.slice/UNIT				"System: UNIT"
 *   user@UID.service/.../UNIT		"Service: UNIT" (the user's own services)
 *   the login session the shell runs in: the process tree. Every process the
 *   shell started (ztmetro's children) is an app's first process, and all its
 *   descendants belong to that app; the app is named by its desktop file
 *   (matched by Exec, file id, icon or WM class) or by its program. SDK apps,
 *   forked from ztos-zygote rather than the shell, go with the launcher that
 *   asked for them (the shell's child that started just before them).
 *   Unnamed helpers of the shell, and ztmetro itself:	"ztMETRO"
 *   other login sessions (ssh)			"Login: sshd-session"
 *
 * CPU time: every process's utime + stime from /proc/PID/stat at each scan
 * (every 15 s with the screen on, 60 s off), and taskstats exit records (a
 * thread's own CPU time, sent by the kernel as it exits) for what ran between
 * the last scan and the exit, and for processes that lived and died between
 * two scans; those go to their parent's app. An app's tallies are its own,
 * not its processes': they stay when the processes are gone.
 */
#include "acctd.h"
#include "acct-common.h"
#include "../ztos-user.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/taskstats.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <unistd.h>

const char *const net_names[NCLS] = { "wifi", "mobile", "other" };
const char *const e_names[NEK] = { "cpu", "net", "screen", "gpu", "base" };

struct agroup groups[MAX_GROUPS];
int n_groups;
struct proc procs[MAX_PROCS];
int n_procs;
struct acct_now now;

#define SCAN_ON_S	15	/* the screen on */
#define SCAN_OFF_S	60	/* off */
#define SAVE_S		(20 * 60)
#define SAVE_MIN_S	(5 * 60)	/* the screen going off saves, at most this often */
#define STATE_MAGIC	0x5a54414343543031ULL	/* "ZTACCT01" */

static uint32_t gen;
static pid_t shell_pid, zygote_pid;
static char boot_id[40];
static time_t charge_since, started_at;
static bool first_scan = true;
static int g_kernel = -1, g_shell = -1, g_short = -1, g_unattr_net = -1;
static unsigned long wakes, scans, exits_seen, exits_lost;
static double hz = 100;

static void say(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

double tick_ticks(void) { return hz; }

static double clock_s(clockid_t c) {
	struct timespec ts;
	clock_gettime(c, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static ssize_t read_text(const char *path, char *buf, size_t size) {
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		buf[0] = 0;
		return -1;
	}
	ssize_t n = read(fd, buf, size - 1);
	close(fd);
	if (n < 0) n = 0;
	buf[n] = 0;
	return n;
}

static const char *base_name(const char *path) {
	const char *s = strrchr(path, '/');
	return s ? s + 1 : path;
}

/* ------------------------------------------------------------ tallies */

void tally_add(struct tally *to, const struct tally *d) {
	to->cpu_s += d->cpu_s;
	to->fg_s += d->fg_s;
	to->bg_cpu_s += d->bg_cpu_s;
	for (int i = 0; i < NCLS; i++) {
		to->rx[i] += d->rx[i];
		to->tx[i] += d->tx[i];
	}
	for (int i = 0; i < NEK; i++) to->e[i] += d->e[i];
}

static double tally_e(const struct tally *t) {
	double s = 0;
	for (int i = 0; i < NEK; i++) s += t->e[i];
	return s;
}

static double tally_bytes(const struct tally *t) {
	double s = 0;
	for (int i = 0; i < NCLS; i++) s += t->rx[i] + t->tx[i];
	return s;
}

static bool tally_zero(const struct tally *t) {
	static const struct tally z;
	return memcmp(t, &z, sizeof(z)) == 0;
}

static int32_t hour_now(void) { return (int32_t)(now.wall / 3600); }

static int32_t day_of(time_t t) {
	struct tm tm;
	localtime_r(&t, &tm);
	return (int32_t)((t + tm.tm_gmtoff) / 86400);
}

/* ------------------------------------------------------------- groups */

int group_get(const char *name, const char *id, char kind) {
	for (int i = 0; i < n_groups; i++) {
		if (strcmp(groups[i].name, name) == 0) {
			return i;
		}
	}
	if (n_groups == MAX_GROUPS) {
		/* full: the least used one that has nothing running goes */
		int worst = -1;
		double least = INFINITY;
		for (int i = 0; i < n_groups; i++) {
			const struct agroup *g = &groups[i];
			if (g->live || g->kind == K_GAUGE || g->kind == K_MODEL || g->kind == K_KERNEL ||
					i == g_shell || i == g_short || i == g_unattr_net) {
				continue;
			}
			double use = tally_e(&g->chg) + g->chg.cpu_s + tally_bytes(&g->chg) / 1e6;
			if (use < least) {
				least = use;
				worst = i;
			}
		}
		if (worst < 0) {
			return g_short >= 0 ? g_short : 0;
		}
		say("ztacctd: forgetting %s for %s", groups[worst].name, name);
		/* nobody may point at it any more */
		for (int i = 0; i < n_procs; i++) if (procs[i].g == worst) procs[i].g = g_short;
		memset(&groups[worst], 0, sizeof(groups[worst]));
		snprintf(groups[worst].name, sizeof(groups[worst].name), "%s", name);
		snprintf(groups[worst].id, sizeof(groups[worst].id), "%s", id ? id : "");
		groups[worst].kind = kind;
		return worst;
	}
	struct agroup *g = &groups[n_groups];
	memset(g, 0, sizeof(*g));
	snprintf(g->name, sizeof(g->name), "%s", name);
	snprintf(g->id, sizeof(g->id), "%s", id ? id : "");
	g->kind = kind;
	return n_groups++;
}

void group_add(int gi, const struct tally *d) {
	if (gi < 0 || gi >= n_groups) {
		return;
	}
	struct agroup *g = &groups[gi];
	tally_add(&g->chg, d);
	int32_t h = hour_now(), dd = day_of(now.wall);
	int hs = ((h % HOURS) + HOURS) % HOURS, ds = ((dd % DAYS) + DAYS) % DAYS;
	if (g->hour_at[hs] != h) {
		memset(&g->hour[hs], 0, sizeof(g->hour[hs]));
		g->hour_at[hs] = h;
	}
	tally_add(&g->hour[hs], d);
	if (g->day_at[ds] != dd) {
		memset(&g->day[ds], 0, sizeof(g->day[ds]));
		g->day_at[ds] = dd;
	}
	tally_add(&g->day[ds], d);
	g->last_active = now.wall;
}

/* window: 0 since charge, 1 today, 2 last 24 h, 3+N: N days ago */
static void group_window(const struct agroup *g, int window, struct tally *out) {
	memset(out, 0, sizeof(*out));
	if (window == 0) {
		*out = g->chg;
	} else if (window == 2) {
		int32_t h = hour_now();
		for (int i = 0; i < HOURS; i++) {
			if (g->hour_at[i] > h - HOURS && g->hour_at[i] <= h) tally_add(out, &g->hour[i]);
		}
	} else {
		int32_t want = day_of(now.wall) - (window == 1 ? 0 : window - 3);
		for (int i = 0; i < DAYS; i++) {
			if (g->day_at[i] == want) tally_add(out, &g->day[i]);
		}
	}
}

int group_unattributed_net(void) { return g_unattr_net; }

/* ------------------------------------------------------ desktop files */

struct desktop {
	char id[64], name[64], exec[48], icon[64], wmclass[64], pkg[96];
};
static struct desktop *desks;
static int n_desks;
static time_t desks_read;

static void desktop_dir(const char *dir) {
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		size_t len = strlen(e->d_name);
		if (len < 9 || strcmp(e->d_name + len - 8, ".desktop") != 0) {
			continue;
		}
		char path[512];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		FILE *f = fopen(path, "re");
		if (!f) {
			continue;
		}
		struct desktop de = { 0 };
		snprintf(de.id, sizeof(de.id), "%.*s", (int)(len - 8), e->d_name);
		char line[1024];
		bool in_entry = false, hidden = false;
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = 0;
			if (line[0] == '[') {
				in_entry = strcmp(line, "[Desktop Entry]") == 0;
				continue;
			}
			if (!in_entry) continue;
			if (!strncmp(line, "Name=", 5) && !de.name[0]) {
				snprintf(de.name, sizeof(de.name), "%s", line + 5);
			} else if (!strncmp(line, "Icon=", 5)) {
				snprintf(de.icon, sizeof(de.icon), "%s", base_name(line + 5));
			} else if (!strncmp(line, "StartupWMClass=", 15)) {
				snprintf(de.wmclass, sizeof(de.wmclass), "%s", line + 15);
				for (char *c = de.wmclass; *c; c++) *c = tolower((unsigned char)*c);
			} else if (!strncmp(line, "Hidden=true", 11)) {
				hidden = true;
			} else if (!strncmp(line, "Exec=", 5)) {
				/* the program: past env, VAR=value and sh -c */
				char *s = line + 5, *tok, *save = NULL;
				bool after_c = false;
				for (tok = strtok_r(s, " \t\"'", &save); tok; tok = strtok_r(NULL, " \t\"'", &save)) {
					if (!strcmp(tok, "env") || strchr(tok, '=') || (tok[0] == '-' && !after_c)) {
						if (!strcmp(tok, "-c")) after_c = true;
						continue;
					}
					const char *b = base_name(tok);
					if (!strcmp(b, "sh") || !strcmp(b, "bash") || !strcmp(b, "exec")) continue;
					if (!strcmp(b, "waydroid")) {
						/* waydroid app launch PACKAGE */
						char *a = strtok_r(NULL, " \t", &save), *l = strtok_r(NULL, " \t", &save);
						char *p = strtok_r(NULL, " \t", &save);
						if (a && l && p && !strcmp(a, "app") && !strcmp(l, "launch")) {
							snprintf(de.pkg, sizeof(de.pkg), "%s", p);
						}
					}
					snprintf(de.exec, sizeof(de.exec), "%s", b);
					break;
				}
			}
		}
		fclose(f);
		if (hidden || !de.name[0]) {
			continue;
		}
		struct desktop *more = realloc(desks, (n_desks + 1) * sizeof(*desks));
		if (!more) break;
		desks = more;
		desks[n_desks++] = de;
	}
	if (d) closedir(d);
}

static void desktop_load(void) {
	free(desks);
	desks = NULL;
	n_desks = 0;
	char path[600];
	snprintf(path, sizeof(path), "%s/.local/share/applications", ztos_user_home());
	desktop_dir(path);	/* first: the user's own override the rest */
	desktop_dir("/usr/local/share/applications");
	desktop_dir("/usr/share/applications");
	desks_read = now.wall;
}

static const struct desktop *desktop_match(const char *const *cands, int n) {
	/* by program first, then by file id, WM class and icon */
	for (int pass = 0; pass < 2; pass++) {
		for (int c = 0; c < n; c++) {
			if (!cands[c] || !cands[c][0]) continue;
			for (int i = 0; i < n_desks; i++) {
				const struct desktop *d = &desks[i];
				if (d->pkg[0]) continue;	/* Android ones go by package */
				if (pass == 0 ? !strcmp(d->exec, cands[c]) :
						(!strcasecmp(d->id, cands[c]) || !strcmp(d->wmclass, cands[c]) ||
						 !strcmp(d->icon, cands[c]))) {
					return d;
				}
			}
		}
	}
	return NULL;
}

static const char *desktop_package(const char *pkg) {
	for (int i = 0; i < n_desks; i++) {
		if (desks[i].pkg[0] && !strcmp(desks[i].pkg, pkg)) return desks[i].name;
	}
	return NULL;
}

/* ---------------------------------------------------------- processes */

static int *phash;	/* pid -> index in procs, open addressing */
#define PHASH 8192

static void phash_build(void) {
	if (!phash) phash = malloc(PHASH * sizeof(int));
	for (int i = 0; i < PHASH; i++) phash[i] = -1;
	for (int i = 0; i < n_procs; i++) {
		if (procs[i].gone) continue;
		unsigned h = (unsigned)procs[i].pid * 2654435761u % PHASH;
		while (phash[h] >= 0) h = (h + 1) % PHASH;
		phash[h] = i;
	}
}

struct proc *proc_find(pid_t pid) {
	if (!phash) return NULL;
	unsigned h = (unsigned)pid * 2654435761u % PHASH;
	while (phash[h] >= 0) {
		if (procs[phash[h]].pid == pid) return &procs[phash[h]];
		h = (h + 1) % PHASH;
	}
	return NULL;
}

/* recently exited pids, for records and packets that come after them */
#define RECENT 512
static struct { pid_t pid; int g; double at; } recent[RECENT];
static int recent_at;

static void recent_add(pid_t pid, int g) {
	recent[recent_at].pid = pid;
	recent[recent_at].g = g;
	recent[recent_at].at = now.mono_s;
	recent_at = (recent_at + 1) % RECENT;
}

int group_of_pid(pid_t pid) {
	struct proc *p = proc_find(pid);
	if (p) return p->g;
	for (int i = 0; i < RECENT; i++) {
		if (recent[i].pid == pid && now.mono_s - recent[i].at < 1800) return recent[i].g;
	}
	return -1;
}

struct pstat {
	pid_t pid, ppid;
	char comm[24];
	char state;
	unsigned long long ticks, start;
	int cpu, threads;
	unsigned flags;
};

static bool read_stat(pid_t pid, struct pstat *s) {
	char path[64], buf[1024];
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	if (read_text(path, buf, sizeof(buf)) <= 0) return false;
	char *open = strchr(buf, '('), *close = strrchr(buf, ')');
	if (!open || !close) return false;
	memset(s, 0, sizeof(*s));
	s->pid = pid;
	snprintf(s->comm, sizeof(s->comm), "%.*s", (int)(close - open - 1), open + 1);
	/* fields from 3 (state) on */
	char *f = close + 2;
	unsigned long long v[42] = { 0 };
	s->state = *f;
	int i = 3;
	for (char *t = f; *t && i < 40; i++) {
		t = strchr(t, ' ');
		if (!t) break;
		t++;
		v[i + 1] = strtoull(t, NULL, 10);
	}
	s->ppid = (pid_t)v[4];
	s->flags = (unsigned)v[9];
	s->ticks = v[14] + v[15];
	s->threads = (int)v[20];
	s->start = v[22];
	s->cpu = (int)v[39];
	return true;
}

static pid_t ppid_of(pid_t pid) {
	struct proc *p = proc_find(pid);
	if (p) return p->ppid;
	struct pstat s;
	return read_stat(pid, &s) ? s.ppid : 0;
}

static unsigned long long start_of(pid_t pid) {
	struct proc *p = proc_find(pid);
	if (p) return p->start;
	struct pstat s;
	return read_stat(pid, &s) ? s.start : 0;
}

static void read_cmdline(pid_t pid, char *out, size_t size, int *argc_out, const char **argv1) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	ssize_t n = read_text(path, out, size);
	int argc = 0;
	if (argv1) *argv1 = NULL;
	for (ssize_t i = 0; i < n; i++) {
		if (out[i] == 0) {
			argc++;
			if (argc == 1 && argv1 && i + 1 < n) *argv1 = out + i + 1;
		}
	}
	if (argc_out) *argc_out = argc;
}

static void read_exe(pid_t pid, char *out, size_t size) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/exe", pid);
	ssize_t n = readlink(path, out, size - 1);
	out[n > 0 ? n : 0] = 0;
	char *del = strstr(out, " (deleted)");
	if (del) *del = 0;
}

static void read_cgroup(pid_t pid, char *out, size_t size) {
	char path[64], buf[1024];
	snprintf(path, sizeof(path), "/proc/%d/cgroup", pid);
	read_text(path, buf, sizeof(buf));
	char *p = strstr(buf, "0::");
	snprintf(out, size, "%s", p ? p + 3 : "");
	out[strcspn(out, "\n")] = 0;
}

static uid_t read_uid(pid_t pid) {
	char path[64], buf[2048];
	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	read_text(path, buf, sizeof(buf));
	char *u = strstr(buf, "\nUid:");
	return u ? (uid_t)strtoul(u + 5, NULL, 10) : (uid_t)-1;
}

static bool is_interpreter(const char *b) {
	return !strcmp(b, "sh") || !strcmp(b, "bash") || !strcmp(b, "dash") || !strncmp(b, "python", 6) ||
		!strcmp(b, "perl") || !strcmp(b, "node") || !strcmp(b, "env");
}

/* The group of an app whose first process is `root`. */
static int group_of_root(pid_t root) {
	char exe[PATH_MAX], cmd[2048], comm[24] = "";
	const char *argv1 = NULL;
	struct pstat s;
	if (read_stat(root, &s)) snprintf(comm, sizeof(comm), "%s", s.comm);
	read_exe(root, exe, sizeof(exe));
	read_cmdline(root, cmd, sizeof(cmd), NULL, &argv1);
	const char *b_exe = base_name(exe), *b_argv0 = base_name(cmd);
	const char *b_argv1 = argv1 && is_interpreter(b_argv0) ? base_name(argv1) : NULL;
	if (b_argv1 && b_argv1[0] == '-') b_argv1 = NULL;
	const char *cands[] = { b_argv1, b_exe, b_argv0, comm };
	const struct desktop *d = desktop_match(cands, 4);
	if (d) {
		return group_get(d->name, d->id, K_APP);
	}
	/* the shell's own helpers (ztos-*, zrn-*, image loaders) are the shell's */
	const char *who = b_argv1 ? b_argv1 : b_exe[0] ? b_exe : comm;
	if (!strncmp(who, "zt", 2) || !strncmp(who, "zrn", 3) || !strcmp(who, "bwrap") || !who[0] ||
			!strcmp(comm, "bwrap") || !strncmp(comm, "zt", 2)) {
		return g_shell;
	}
	return group_get(who, who, K_APP);
}

/* An SDK app is forked by ztos-zygote, not by the shell: it goes with the
 * shell's child (its launcher) that started last before it. */
static pid_t zygote_launcher(pid_t child) {
	unsigned long long cs = start_of(child), best_start = 0;
	pid_t best = 0;
	for (int i = 0; i < n_procs; i++) {
		const struct proc *p = &procs[i];
		if (p->ppid == shell_pid && p->pid != child && p->start <= cs && p->start + 10 * hz >= cs &&
				p->start >= best_start) {
			best = p->pid;
			best_start = p->start;
		}
	}
	return best;
}

static void scope_app_id(const char *leaf, char *out, size_t size) {
	/* app-ID.scope, app-ID-1234.scope, app-gnome-ID-1234.scope */
	snprintf(out, size, "%s", leaf + 4);
	char *dot = strstr(out, ".scope");
	if (dot) *dot = 0;
	char *dash = strrchr(out, '-');
	if (dash && dash[1] && strspn(dash + 1, "0123456789") == strlen(dash + 1)) *dash = 0;
}

static int classify(pid_t pid, const struct pstat *s, pid_t *root_out) {
	*root_out = 0;
	if (pid == 2 || s->ppid == 2 || (s->flags & 0x00200000)) {	/* PF_KTHREAD */
		return g_kernel;
	}
	char cg[512];
	read_cgroup(pid, cg, sizeof(cg));
	if (strstr(cg, "/lxc.payload.waydroid") == cg || strstr(cg, "lxc.payload.waydroid")) {
		uid_t uid = read_uid(pid);
		if (uid != (uid_t)-1 && uid % 100000 >= 10000) {
			char cmd[256];
			read_cmdline(pid, cmd, sizeof(cmd), NULL, NULL);
			cmd[strcspn(cmd, ":")] = 0;	/* com.foo:service is com.foo */
			if (cmd[0] && strchr(cmd, '.')) {
				const char *label = desktop_package(cmd);
				char name[64];
				snprintf(name, sizeof(name), "%s", label ? label : cmd);
				return group_get(name, cmd, K_ANDROID);
			}
		}
		return group_get("Android system", "waydroid", K_ANDROID);
	}
	/* a cgroup made for one app */
	char *tickle = strstr(cg, "/zrn-tickle-");
	if (tickle) {
		pid_t root = atoi(tickle + 12);
		if (root > 0 && root != pid) {
			struct proc *rp = proc_find(root);
			*root_out = root;
			if (rp) return rp->g;
			if (kill(root, 0) == 0) return group_of_root(root);
		} else if (root == pid) {
			*root_out = pid;
			return group_of_root(pid);
		}
	}
	char *leaf = strrchr(cg, '/');
	leaf = leaf ? leaf + 1 : cg;
	if (!strncmp(leaf, "app-", 4) && strstr(leaf, ".scope")) {
		char id[96];
		scope_app_id(leaf, id, sizeof(id));
		const char *cands[] = { id };
		const struct desktop *d = desktop_match(cands, 1);
		return d ? group_get(d->name, d->id, K_APP) : group_get(id, id, K_APP);
	}
	if (!strncmp(cg, "/system.slice/", 14)) {
		char unit[96];
		snprintf(unit, sizeof(unit), "%s", cg + 14);
		unit[strcspn(unit, "/")] = 0;
		char *dot = strstr(unit, ".service");
		if (dot) *dot = 0;
		char name[64];
		snprintf(name, sizeof(name), "System: %s", unit);
		return group_get(name, unit, K_SERVICE);
	}
	if (!strcmp(cg, "/init.scope") || !strcmp(cg, "/")) {
		return group_get("System: init", "init.scope", K_SERVICE);
	}
	char *user = strstr(cg, ".service/");
	if (user && strstr(cg, "/user@")) {
		/* user@1000.service/app.slice/zrn-pushd.service, .../init.scope */
		char unit[96];
		const char *last = strrchr(cg, '/') + 1;
		if (!strcmp(last, "init.scope")) {
			snprintf(unit, sizeof(unit), "systemd --user");
		} else {
			snprintf(unit, sizeof(unit), "%s", last);
			char *dot = strstr(unit, ".service");
			if (dot) *dot = 0;
		}
		char name[64];
		snprintf(name, sizeof(name), "Service: %s", unit);
		return group_get(name, unit, K_USER);
	}
	/* a login session: the shell's, or another (ssh) */
	if (pid == shell_pid) {
		return g_shell;
	}
	pid_t p = pid, below = 0, top = pid;
	for (int depth = 0; p > 1 && depth < 64; depth++) {
		if (p == shell_pid) {
			*root_out = below;
			struct proc *rp = proc_find(below);
			return rp && rp->root == below && rp->g >= 0 ? rp->g : group_of_root(below);
		}
		if (p == zygote_pid && below) {
			/* forked by the warm runtime: the launcher's app */
			pid_t l = zygote_launcher(below);
			if (l) {
				*root_out = l;
				struct proc *lp = proc_find(l);
				return lp ? lp->g : group_of_root(l);
			}
			return g_shell;
		}
		below = p;
		top = p;
		p = ppid_of(p);
	}
	if (top == zygote_pid) return g_shell;
	char name[64], comm[24] = "";
	struct pstat ts;
	if (read_stat(top, &ts)) snprintf(comm, sizeof(comm), "%s", ts.comm);
	if (!strncmp(comm, "sshd", 4)) snprintf(comm, sizeof(comm), "ssh");
	snprintf(name, sizeof(name), "Login: %s", comm[0] ? comm : "?");
	return group_get(name, comm, K_LOGIN);
}

static void find_shell(void) {
	/* ztmetro and ztos-zygote: their children are apps */
	DIR *d = opendir("/proc");
	struct dirent *e;
	shell_pid = zygote_pid = 0;
	while (d && (e = readdir(d))) {
		pid_t pid = atoi(e->d_name);
		if (pid <= 0) continue;
		char path[64], comm[32];
		snprintf(path, sizeof(path), "/proc/%d/comm", pid);
		read_text(path, comm, sizeof(comm));
		comm[strcspn(comm, "\n")] = 0;
		if (!strcmp(comm, "ztmetro")) {
			char cmd[256];
			read_cmdline(pid, cmd, sizeof(cmd), NULL, NULL);
			if (!shell_pid || pid < shell_pid) shell_pid = pid;
		} else if (!strncmp(comm, "python", 6)) {
			char cmd[512];
			const char *a1 = NULL;
			int argc;
			read_cmdline(pid, cmd, sizeof(cmd), &argc, &a1);
			/* python3 -I .../zygote.py */
			for (const char *a = a1; a && a < cmd + sizeof(cmd) && *a; a += strlen(a) + 1) {
				if (strstr(a, "zygote.py")) zygote_pid = pid;
			}
		}
	}
	if (d) closedir(d);
}

/* a process that is gone: what ran since the last scan (its threads' exit
 * records), then its name and tallies into its app's exited processes */
static void proc_gone(struct proc *p) {
	if (p->exit_since > 0) {
		struct tally d = { .cpu_s = p->exit_since };
		if (!(now.screen_on && now.front_g == p->g)) d.bg_cpu_s = d.cpu_s;
		p->t.cpu_s += d.cpu_s;
		p->d_cpu += d.cpu_s;
		group_add(p->g, &d);
		p->exit_since = 0;
	}
	if (p->g < 0 || p->g >= n_groups) return;
	struct agroup *g = &groups[p->g];
	tally_add(&g->exited_t, &p->t);
	g->n_exited++;
	int slot = -1, small = 0;
	for (int i = 0; i < EXITED_KEEP; i++) {
		if (!strcmp(g->ex[i].comm, p->comm) || !g->ex[i].comm[0]) {
			slot = i;
			break;
		}
		if (tally_e(&g->ex[i].t) + g->ex[i].t.cpu_s < tally_e(&g->ex[small].t) + g->ex[small].t.cpu_s) small = i;
	}
	if (slot < 0) {
		/* the smallest name makes room, folded into the app's "other" */
		slot = small;
		memset(&g->ex[slot], 0, sizeof(g->ex[slot]));
	}
	snprintf(g->ex[slot].comm, sizeof(g->ex[slot].comm), "%s", p->comm);
	g->ex[slot].n++;
	tally_add(&g->ex[slot].t, &p->t);
	recent_add(p->pid, p->g);
}

/* saved baselines of the same boot: a restart of the daemon loses nothing */
struct saved_proc {
	pid_t pid;
	unsigned long long start, ticks;
	char group[64];
	struct tally t;
	double e_raw;
};
static struct saved_proc *saved;
static int n_saved;

static void scan(void) {
	gen++;
	scans++;
	if (!shell_pid || kill(shell_pid, 0) < 0 || (zygote_pid && kill(zygote_pid, 0) < 0) || scans % 20 == 1) {
		find_shell();
	}
	for (int i = 0; i < n_groups; i++) groups[i].live = 0;
	DIR *d = opendir("/proc");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		pid_t pid = atoi(e->d_name);
		if (pid <= 0) continue;
		struct pstat s;
		if (!read_stat(pid, &s)) continue;
		struct proc *p = proc_find(pid);
		if (p && p->start != s.start) {
			proc_gone(p);	/* the pid is someone else's now */
			p->gone = true;
			p = NULL;
		}
		if (p && !p->gone && strcmp(p->comm, s.comm) && p->root == pid && p->g != g_kernel) {
			/* an app's first process has become its program (sh -c exec): name it again */
			int was = p->g, now_g = group_of_root(pid);
			if (now_g != was) {
				for (int i = 0; i < n_procs; i++) {
					if (procs[i].root == pid) procs[i].g = now_g;
				}
			}
		}
		if (!p || p->gone) {
			if (n_procs == MAX_PROCS) continue;
			p = &procs[n_procs++];
			memset(p, 0, sizeof(*p));
			p->pid = pid;
			p->start = s.start;
			p->ppid = s.ppid;
			p->kthread = pid == 2 || s.ppid == 2;
			snprintf(p->comm, sizeof(p->comm), "%s", s.comm);
			p->g = classify(pid, &s, &p->root);
			if (!p->root && p->g != g_kernel && s.ppid == shell_pid) p->root = pid;
			p->ticks = s.ticks;
			for (int i = 0; i < n_saved; i++) {
				if (saved[i].pid == pid && saved[i].start == s.start) {
					p->ticks = saved[i].ticks;
					p->t = saved[i].t;
					p->e_raw = saved[i].e_raw;
					break;
				}
			}
			/* a process started since the last scan: all of its time is new.
			 * At the first scan of a boot nothing older is counted. */
			if (!first_scan && p->ticks == s.ticks) {
				p->ticks = 0;
			}
			phash_build();	/* so its children find it in this scan */
		}
		p->gen = gen;
		p->ppid = s.ppid;
		p->cpu = s.cpu;
		p->threads = s.threads;
		snprintf(p->comm, sizeof(p->comm), "%s", s.comm);
		double dcpu = s.ticks >= p->ticks ? (s.ticks - p->ticks) / hz : 0;
		p->ticks = s.ticks;
		p->exit_since = 0;	/* /proc's count has them now */
		p->d_cpu = dcpu;
		if (dcpu > 0) {
			struct tally dt = { .cpu_s = dcpu };
			if (!(now.screen_on && now.front_g == p->g)) dt.bg_cpu_s = dcpu;
			p->t.cpu_s += dcpu;
			p->t.bg_cpu_s += dt.bg_cpu_s;
			group_add(p->g, &dt);
		}
		if (p->g >= 0 && p->g < n_groups) groups[p->g].live++;
	}
	if (d) closedir(d);
	/* the ones gone */
	int w = 0;
	for (int i = 0; i < n_procs; i++) {
		if (procs[i].gen != gen || procs[i].gone) {
			if (!procs[i].gone) proc_gone(&procs[i]);
			continue;
		}
		if (w != i) procs[w] = procs[i];
		w++;
	}
	n_procs = w;
	phash_build();
	if (first_scan) {
		first_scan = false;
		free(saved);
		saved = NULL;
		n_saved = 0;
	}
}

/* ---------------------------------------------------------- taskstats */

static int ts_fd = -1, ts_family;

static int nl_send(int fd, uint16_t type, uint8_t cmd, uint16_t attr, const void *data, int len) {
	struct {
		struct nlmsghdr n;
		struct genlmsghdr g;
		char buf[256];
	} req = { 0 };
	req.n.nlmsg_type = type;
	req.n.nlmsg_flags = NLM_F_REQUEST;
	req.n.nlmsg_pid = getpid();
	req.g.cmd = cmd;
	req.g.version = 1;
	struct nlattr *na = (struct nlattr *)req.buf;
	na->nla_type = attr;
	na->nla_len = NLA_HDRLEN + len;
	memcpy((char *)na + NLA_HDRLEN, data, len);
	req.n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN) + NLA_ALIGN(na->nla_len);
	struct sockaddr_nl to = { .nl_family = AF_NETLINK };
	return sendto(fd, &req, req.n.nlmsg_len, 0, (struct sockaddr *)&to, sizeof(to)) < 0 ? -1 : 0;
}

static bool taskstats_start(void) {
	ts_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
	if (ts_fd < 0) return false;
	struct sockaddr_nl a = { .nl_family = AF_NETLINK };
	if (bind(ts_fd, (struct sockaddr *)&a, sizeof(a)) < 0) goto fail;
	int big = 4 << 20;
	setsockopt(ts_fd, SOL_SOCKET, SO_RCVBUFFORCE, &big, sizeof(big));
	if (nl_send(ts_fd, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, CTRL_ATTR_FAMILY_NAME, TASKSTATS_GENL_NAME,
			strlen(TASKSTATS_GENL_NAME) + 1) < 0) goto fail;
	char buf[4096];
	ssize_t n = recv(ts_fd, buf, sizeof(buf), 0);
	struct nlmsghdr *h = (struct nlmsghdr *)buf;
	if (n <= 0 || !NLMSG_OK(h, (size_t)n) || h->nlmsg_type == NLMSG_ERROR) goto fail;
	struct nlattr *na = (struct nlattr *)((char *)NLMSG_DATA(h) + GENL_HDRLEN);
	int left = h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
	while (left > (int)NLA_HDRLEN) {
		if (na->nla_type == CTRL_ATTR_FAMILY_ID) ts_family = *(uint16_t *)((char *)na + NLA_HDRLEN);
		left -= NLA_ALIGN(na->nla_len);
		na = (struct nlattr *)((char *)na + NLA_ALIGN(na->nla_len));
	}
	if (!ts_family) goto fail;
	char mask[32];
	snprintf(mask, sizeof(mask), "0-%ld", sysconf(_SC_NPROCESSORS_CONF) - 1);
	if (nl_send(ts_fd, ts_family, TASKSTATS_CMD_GET, TASKSTATS_CMD_ATTR_REGISTER_CPUMASK, mask, strlen(mask) + 1) < 0)
		goto fail;
	fcntl(ts_fd, F_SETFL, O_NONBLOCK);
	return true;
fail:
	say("ztacctd: taskstats: %s", strerror(errno));
	close(ts_fd);
	ts_fd = -1;
	return false;
}

/* one thread has exited: its own CPU time */
static void task_exited(const struct taskstats *t) {
	exits_seen++;
	double cpu = (t->ac_utime + t->ac_stime) / 1e6;
	pid_t tgid = t->ac_tgid ? (pid_t)t->ac_tgid : (pid_t)t->ac_pid;
	struct proc *p = proc_find(tgid);
	if (p && !p->gone) {
		p->exit_since += cpu;
		return;
	}
	if (cpu <= 0) return;
	/* born and gone between two scans: its parent's app */
	int g = -1;
	if (t->ac_ppid == 2 || tgid == 2) {
		g = g_kernel;
	} else {
		g = group_of_pid(tgid);
		if (g < 0) g = group_of_pid(t->ac_ppid);
		if (g < 0 && t->ac_ppid > 1) {
			struct pstat s;
			if (read_stat(t->ac_ppid, &s)) {
				pid_t root;
				g = classify(t->ac_ppid, &s, &root);
			}
		}
	}
	if (g < 0) g = g_short;
	struct tally d = { .cpu_s = cpu };
	if (!(now.screen_on && now.front_g == g)) d.bg_cpu_s = cpu;
	group_add(g, &d);
	if (t->ac_pid == (uint32_t)tgid) {
		/* the whole short life of a process, for its app's exited list */
		struct agroup *gr = &groups[g];
		gr->exited_t.cpu_s += cpu;
		gr->n_exited++;
		recent_add(tgid, g);
		int slot = -1;
		for (int i = 0; i < EXITED_KEEP; i++) {
			if (!strncmp(gr->ex[i].comm, t->ac_comm, sizeof(gr->ex[i].comm) - 1) || !gr->ex[i].comm[0]) {
				slot = i;
				break;
			}
		}
		if (slot >= 0) {
			snprintf(gr->ex[slot].comm, sizeof(gr->ex[slot].comm), "%.23s", t->ac_comm);
			gr->ex[slot].n++;
			gr->ex[slot].t.cpu_s += cpu;
		}
	}
}

static void taskstats_read(void) {
	char buf[16384];
	for (;;) {
		ssize_t n = recv(ts_fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (n < 0) {
			if (errno == ENOBUFS) {
				exits_lost++;
				continue;
			}
			return;
		}
		for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (size_t)n); h = NLMSG_NEXT(h, n)) {
			if (h->nlmsg_type != ts_family) continue;
			struct nlattr *na = (struct nlattr *)((char *)NLMSG_DATA(h) + GENL_HDRLEN);
			int left = h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
			while (left >= (int)NLA_HDRLEN && na->nla_len >= NLA_HDRLEN) {
				if (na->nla_type == TASKSTATS_TYPE_AGGR_PID) {
					struct nlattr *in = (struct nlattr *)((char *)na + NLA_HDRLEN);
					int il = na->nla_len - NLA_HDRLEN;
					while (il >= (int)NLA_HDRLEN && in->nla_len >= NLA_HDRLEN) {
						if (in->nla_type == TASKSTATS_TYPE_STATS) {
							struct taskstats t = { 0 };
							int len = in->nla_len - NLA_HDRLEN;
							memcpy(&t, (char *)in + NLA_HDRLEN, len < (int)sizeof(t) ? len : (int)sizeof(t));
							task_exited(&t);
						}
						il -= NLA_ALIGN(in->nla_len);
						in = (struct nlattr *)((char *)in + NLA_ALIGN(in->nla_len));
					}
				}
				left -= NLA_ALIGN(na->nla_len);
				na = (struct nlattr *)((char *)na + NLA_ALIGN(na->nla_len));
			}
		}
	}
}

/* --------------------------------------------- the screen and the front */

static time_t last_save;
static void save_state(void);

static void read_front(void) {
	char buf[4096];
	read_text("/run/zrnperfd.status", buf, sizeof(buf));
	bool on = now.screen_on, known = false;
	pid_t front = 0;
	char *s = strstr(buf, "screen=");
	if (s) {
		on = !strncmp(s + 7, "on", 2);
	} else {
		char bl[32];
		read_text("/sys/class/backlight/s9p-panel/bl_power", bl, sizeof(bl));
		on = atoi(bl) == 0;
	}
	char *f = strstr(buf, "\nfront=");
	if (f) {
		known = true;
		front = atoi(f + 7);
	}
	int fg = g_shell;
	if (front > 0) {
		struct proc *p = proc_find(front);
		if (p) {
			fg = p->g;
		} else {
			struct pstat ps;
			if (read_stat(front, &ps)) {
				pid_t root;
				fg = classify(front, &ps, &root);
			}
		}
	}
	if (on != now.screen_on || fg != now.front_g || known != now.front_known) {
		energy_screen_change();	/* the time until now goes to the old ones */
		bool went_off = now.screen_on && !on;
		now.screen_on = on;
		now.front_pid = front;
		now.front_g = fg;
		now.front_known = known;
		if (went_off && now.wall - last_save >= SAVE_MIN_S) {
			save_state();
		}
	}
}

/* -------------------------------------------------------- the charger */

static bool was_plugged = false;
static int full_seen;

static void read_charger(void) {
	char st[32], cap[16], onl[16];
	read_text("/sys/class/power_supply/max170xx_battery/status", st, sizeof(st));
	read_text("/sys/class/power_supply/max170xx_battery/capacity", cap, sizeof(cap));
	read_text("/sys/class/power_supply/max77705-charger/online", onl, sizeof(onl));
	bool plugged = atoi(onl) == 1 || !strncmp(st, "Charging", 8) || !strncmp(st, "Full", 4);
	now.charging = plugged;
	if (plugged && (atoi(cap) >= 95 || !strncmp(st, "Full", 4))) full_seen = 1;
	if (was_plugged && !plugged && full_seen) {
		/* unplugged after a full charge: a new "since charge" */
		energy_settle(true);
		for (int i = 0; i < n_groups; i++) {
			memset(&groups[i].chg, 0, sizeof(groups[i].chg));
			memset(&groups[i].exited_t, 0, sizeof(groups[i].exited_t));
			groups[i].n_exited = 0;
			memset(groups[i].ex, 0, sizeof(groups[i].ex));
		}
		for (int i = 0; i < n_procs; i++) {
			memset(&procs[i].t, 0, sizeof(procs[i].t));
			procs[i].e_raw = 0;
		}
		charge_since = now.wall;
		full_seen = 0;
		say("ztacctd: unplugged after a full charge: totals start again");
	}
	was_plugged = plugged;
}

/* ------------------------------------------------------- the state file */

struct state_head {
	uint64_t magic;
	uint32_t version, n_groups, n_procs, pad;
	int64_t saved, charge_since, started;
	char boot_id[40];
	int32_t full_seen, was_plugged;
};

struct state_group {
	char name[64], id[64];
	char kind, pad[7];
	struct tally chg, exited_t;
	uint32_t n_exited, n_slots;	/* then n_slots of struct state_slot */
	struct exited ex[EXITED_KEEP];
	int64_t last_active;
};
struct state_slot {
	int32_t at;
	int32_t is_day;
	struct tally t;
};

static void save_state(void) {
	energy_settle(true);
	mkdir(ACCT_LIB, 0755);
	char tmp[] = ACCT_STATE ".new";
	FILE *f = fopen(tmp, "we");
	if (!f) {
		say("ztacctd: %s: %s", tmp, strerror(errno));
		return;
	}
	/* history kept: apps used in the last 8 days, or with anything since the charge */
	int keep = 0;
	for (int i = 0; i < n_groups; i++) keep++;
	struct state_head h = { .magic = STATE_MAGIC, .version = 1, .n_groups = keep, .n_procs = n_procs,
		.saved = now.wall, .charge_since = charge_since, .started = started_at,
		.full_seen = full_seen, .was_plugged = was_plugged };
	snprintf(h.boot_id, sizeof(h.boot_id), "%s", boot_id);
	fwrite(&h, sizeof(h), 1, f);
	for (int i = 0; i < n_groups; i++) {
		const struct agroup *g = &groups[i];
		struct state_group sg = { .kind = g->kind, .chg = g->chg, .exited_t = g->exited_t,
			.n_exited = g->n_exited, .last_active = g->last_active };
		memcpy(sg.name, g->name, sizeof(sg.name));
		memcpy(sg.id, g->id, sizeof(sg.id));
		memcpy(sg.ex, g->ex, sizeof(sg.ex));
		for (int s = 0; s < HOURS; s++) sg.n_slots += g->hour_at[s] && !tally_zero(&g->hour[s]);
		for (int s = 0; s < DAYS; s++) sg.n_slots += g->day_at[s] && !tally_zero(&g->day[s]);
		fwrite(&sg, sizeof(sg), 1, f);
		for (int s = 0; s < HOURS; s++) {
			if (!g->hour_at[s] || tally_zero(&g->hour[s])) continue;
			struct state_slot sl = { .at = g->hour_at[s], .is_day = 0, .t = g->hour[s] };
			fwrite(&sl, sizeof(sl), 1, f);
		}
		for (int s = 0; s < DAYS; s++) {
			if (!g->day_at[s] || tally_zero(&g->day[s])) continue;
			struct state_slot sl = { .at = g->day_at[s], .is_day = 1, .t = g->day[s] };
			fwrite(&sl, sizeof(sl), 1, f);
		}
	}
	for (int i = 0; i < n_procs; i++) {
		struct saved_proc sp = { .pid = procs[i].pid, .start = procs[i].start, .ticks = procs[i].ticks,
			.t = procs[i].t, .e_raw = procs[i].e_raw };
		if (procs[i].g >= 0 && procs[i].g < n_groups)
			snprintf(sp.group, sizeof(sp.group), "%s", groups[procs[i].g].name);
		fwrite(&sp, sizeof(sp), 1, f);
	}
	bool ok = !ferror(f);
	fflush(f);
	fdatasync(fileno(f));
	ok = fclose(f) == 0 && ok;
	if (ok && rename(tmp, ACCT_STATE) == 0) {
		last_save = now.wall;
	} else {
		say("ztacctd: saving: %s", strerror(errno));
		unlink(tmp);
	}
	energy_save_calib();
}

static void load_state(void) {
	FILE *f = fopen(ACCT_STATE, "re");
	if (!f) return;
	struct state_head h;
	if (fread(&h, sizeof(h), 1, f) != 1 || h.magic != STATE_MAGIC || h.version != 1) {
		say("ztacctd: %s is not ours, starting afresh", ACCT_STATE);
		fclose(f);
		return;
	}
	charge_since = h.charge_since;
	full_seen = h.full_seen;
	was_plugged = h.was_plugged;
	int32_t oldest_day = day_of(now.wall) - DAYS;
	for (uint32_t i = 0; i < h.n_groups; i++) {
		struct state_group sg;
		if (fread(&sg, sizeof(sg), 1, f) != 1) break;
		sg.name[sizeof(sg.name) - 1] = sg.id[sizeof(sg.id) - 1] = 0;
		int gi = group_get(sg.name, sg.id, sg.kind);
		struct agroup *g = &groups[gi];
		g->kind = sg.kind;
		g->chg = sg.chg;
		g->exited_t = sg.exited_t;
		g->n_exited = sg.n_exited;
		memcpy(g->ex, sg.ex, sizeof(g->ex));
		g->last_active = sg.last_active;
		for (uint32_t s = 0; s < sg.n_slots; s++) {
			struct state_slot sl;
			if (fread(&sl, sizeof(sl), 1, f) != 1) break;
			if (sl.is_day) {
				if (sl.at <= oldest_day) continue;
				int ds = ((sl.at % DAYS) + DAYS) % DAYS;
				g->day_at[ds] = sl.at;
				g->day[ds] = sl.t;
			} else {
				if (sl.at <= hour_now() - HOURS) continue;
				int hs = ((sl.at % HOURS) + HOURS) % HOURS;
				g->hour_at[hs] = sl.at;
				g->hour[hs] = sl.t;
			}
		}
	}
	if (!strcmp(h.boot_id, boot_id)) {
		saved = calloc(h.n_procs ? h.n_procs : 1, sizeof(*saved));
		for (uint32_t i = 0; saved && i < h.n_procs; i++) {
			if (fread(&saved[n_saved], sizeof(*saved), 1, f) != 1) break;
			n_saved++;
		}
	}
	fclose(f);
	say("ztacctd: %d apps from %s%s", n_groups, ACCT_STATE, n_saved ? ", processes of this boot too" : "");
}

/* --------------------------------------------------------------- JSON */

struct jb {
	char *p;
	size_t n, cap;
	bool comma;
};

static void jb_raw(struct jb *j, const char *s, size_t len) {
	if (j->n + len + 1 > j->cap) {
		size_t cap = j->cap ? j->cap : 8192;
		while (cap < j->n + len + 1) cap *= 2;
		char *p = realloc(j->p, cap);
		if (!p) return;
		j->p = p;
		j->cap = cap;
	}
	memcpy(j->p + j->n, s, len);
	j->n += len;
	j->p[j->n] = 0;
}

static void jb_printf(struct jb *j, const char *fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	jb_raw(j, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
}

static void jb_sep(struct jb *j) {
	if (j->comma) jb_raw(j, ",", 1);
	j->comma = true;
}

static void jb_esc(struct jb *j, const char *s) {
	jb_raw(j, "\"", 1);
	for (; *s; s++) {
		unsigned char c = *s;
		if (c == '"' || c == '\\') {
			char e[2] = { '\\', c };
			jb_raw(j, e, 2);
		} else if (c < 0x20) {
			jb_printf(j, "\\u%04x", c);
		} else {
			jb_raw(j, (const char *)&c, 1);
		}
	}
	jb_raw(j, "\"", 1);
}

static void jb_key(struct jb *j, const char *k) {
	jb_sep(j);
	jb_esc(j, k);
	jb_raw(j, ":", 1);
}
static void jb_open(struct jb *j, const char *k, char c) {
	if (k) jb_key(j, k); else jb_sep(j);
	jb_raw(j, &c, 1);
	j->comma = false;
}
static void jb_close(struct jb *j, char c) {
	jb_raw(j, &c, 1);
	j->comma = true;
}
static void jb_str(struct jb *j, const char *k, const char *v) {
	if (k) jb_key(j, k); else jb_sep(j);
	jb_esc(j, v ? v : "");
}
static void jb_num(struct jb *j, const char *k, double v) {
	jb_key(j, k);
	if (!isfinite(v)) v = 0;
	if (v == floor(v) && fabs(v) < 1e15) jb_printf(j, "%.0f", v);
	else jb_printf(j, "%.6g", v);
}
static void jb_int(struct jb *j, const char *k, long long v) {
	jb_key(j, k);
	jb_printf(j, "%lld", v);
}

static void jb_tally(struct jb *j, const struct tally *t) {
	jb_num(j, "cpu_s", t->cpu_s);
	jb_num(j, "fg_s", t->fg_s);
	jb_num(j, "bg_cpu_s", t->bg_cpu_s);
	jb_open(j, "rx", '{');
	for (int i = 0; i < NCLS; i++) jb_num(j, net_names[i], t->rx[i]);
	jb_close(j, '}');
	jb_open(j, "tx", '{');
	for (int i = 0; i < NCLS; i++) jb_num(j, net_names[i], t->tx[i]);
	jb_close(j, '}');
	jb_num(j, "wh", tally_e(t) / 3600);
	jb_open(j, "e_wh", '{');
	for (int i = 0; i < NEK; i++) jb_num(j, e_names[i], t->e[i] / 3600);
	jb_close(j, '}');
}

static int parse_window(const char *w) {
	if (!w || !*w || !strcmp(w, "charge")) return 0;
	if (!strcmp(w, "today")) return 1;
	if (!strcmp(w, "24h")) return 2;
	if (!strncmp(w, "day:", 4)) {
		int n = atoi(w + 4);
		return n >= 0 && n < DAYS - 1 ? 3 + n : -1;
	}
	return -1;
}

static const char *window_name(int w) {
	static char buf[16];
	if (w == 0) return "charge";
	if (w == 1) return "today";
	if (w == 2) return "24h";
	snprintf(buf, sizeof(buf), "day:%d", w - 3);
	return buf;
}

static int g_gauge = -1;

static void jb_window_head(struct jb *j, int window) {
	jb_str(j, "window", window_name(window));
	jb_int(j, "now", now.wall);
	jb_int(j, "charge_since", charge_since ? charge_since : started_at);
	struct tally gauge;
	group_window(&groups[g_gauge], window, &gauge);
	/* the gauge's own group: e[0] measured, e[1] modelled (before scaling), fg_s seconds on battery */
	jb_num(j, "measured_wh", gauge.e[0] / 3600);
	jb_num(j, "modelled_wh", gauge.e[1] / 3600);
	jb_num(j, "on_battery_s", gauge.fg_s);
	jb_num(j, "charging_s", gauge.cpu_s);
	jb_int(j, "screen_on", now.screen_on);
	jb_int(j, "front_known", now.front_known);
	if (now.front_g >= 0 && now.front_g < n_groups) jb_str(j, "front", groups[now.front_g].name);
}

/* processes of a group, live, with their part of it */
static void jb_procs(struct jb *j, int gi, const struct tally *gt, bool detail) {
	jb_open(j, "procs", '[');
	double gcpu = groups[gi].chg.cpu_s, ge = tally_e(&groups[gi].chg);
	int shown = 0;
	for (int i = 0; i < n_procs; i++) {
		const struct proc *p = &procs[i];
		if (p->g != gi) continue;
		if (!detail && (shown >= 40 || (p->t.cpu_s <= 0 && tally_bytes(&p->t) <= 0))) continue;
		shown++;
		jb_open(j, NULL, '{');
		jb_int(j, "pid", p->pid);
		jb_int(j, "ppid", p->ppid);
		jb_str(j, "comm", p->comm);
		jb_int(j, "threads", p->threads);
		jb_int(j, "root", p->root == p->pid);
		jb_tally(j, &p->t);
		jb_num(j, "share_cpu", gcpu > 0 ? fmin(1, p->t.cpu_s / gcpu) : 0);
		jb_num(j, "share_wh", ge > 0 ? fmin(1, tally_e(&p->t) / ge) : 0);
		if (detail) {
			char cmd[512];
			int argc;
			read_cmdline(p->pid, cmd, sizeof(cmd), &argc, NULL);
			for (int c = 0; c < (int)sizeof(cmd) - 1; c++) if (!cmd[c] && cmd[c + 1]) cmd[c] = ' ';
			jb_str(j, "cmdline", cmd);
		}
		jb_close(j, '}');
	}
	jb_close(j, ']');
	(void)gt;
}

static int group_sort_window, group_sort_key;
static int group_cmp(const void *a, const void *b) {
	struct tally ta, tb;
	group_window(&groups[*(const int *)a], group_sort_window, &ta);
	group_window(&groups[*(const int *)b], group_sort_window, &tb);
	double va, vb;
	switch (group_sort_key) {
	case 1: va = ta.cpu_s; vb = tb.cpu_s; break;
	case 2: va = tally_bytes(&ta); vb = tally_bytes(&tb); break;
	case 3: va = ta.bg_cpu_s; vb = tb.bg_cpu_s; break;
	default: va = tally_e(&ta) + ta.cpu_s * 1e-6; vb = tally_e(&tb) + tb.cpu_s * 1e-6;
	}
	return va < vb ? 1 : va > vb ? -1 : 0;
}

static void answer_top(struct jb *j, int window, int sort) {
	jb_open(j, NULL, '{');
	jb_window_head(j, window);
	int order[MAX_GROUPS], n = 0;
	for (int i = 0; i < n_groups; i++) {
		if (groups[i].kind == K_GAUGE) continue;
		struct tally t;
		group_window(&groups[i], window, &t);
		if (tally_zero(&t) && !groups[i].live) continue;
		order[n++] = i;
	}
	group_sort_window = window;
	group_sort_key = sort;
	qsort(order, n, sizeof(int), group_cmp);
	jb_open(j, "groups", '[');
	for (int k = 0; k < n; k++) {
		const struct agroup *g = &groups[order[k]];
		struct tally t;
		group_window(g, window, &t);
		jb_open(j, NULL, '{');
		jb_str(j, "name", g->name);
		jb_str(j, "id", g->id);
		char kind[2] = { g->kind, 0 };
		jb_str(j, "kind", kind);
		jb_int(j, "live", g->live);
		jb_int(j, "exited", g->n_exited);
		jb_tally(j, &t);
		jb_procs(j, order[k], &t, false);
		jb_close(j, '}');
	}
	jb_close(j, ']');
	jb_close(j, '}');
}

/* --------------------------------------------------------- open files */

struct ofile {
	char target[256];
	char what[200];
	int n;
	pid_t pids[8];
	int kind;	/* 0 file, 1 socket, 2 pipe, 3 anon, 4 device, 5 other */
};

static const char *const ofile_kinds[] = { "files", "sockets", "pipes", "anon", "devices", "other" };

/* inode -> what a socket is, from /proc/net (the phone's own namespace) */
struct sockinfo { unsigned long ino; char what[160]; };
static struct sockinfo *sinfo;
static int n_sinfo;

static void sock_table(const char *file, const char *proto, bool v6) {
	FILE *f = fopen(file, "re");
	char line[512];
	if (!f) return;
	fgets(line, sizeof(line), f);
	while (fgets(line, sizeof(line), f)) {
		char la[64], ra[64];
		unsigned st;
		unsigned long ino;
		if (sscanf(line, "%*d: %63s %63s %x %*s %*s %*s %*d %*d %lu", la, ra, &st, &ino) != 4) continue;
		char lh[64], rh[64];
		unsigned lp = 0, rp = 0;
		char *c = strchr(la, ':'), *d = strchr(ra, ':');
		if (!c || !d) continue;
		*c = *d = 0;
		lp = strtoul(c + 1, NULL, 16);
		rp = strtoul(d + 1, NULL, 16);
		if (!v6) {
			unsigned a = strtoul(la, NULL, 16), b = strtoul(ra, NULL, 16);
			snprintf(lh, sizeof(lh), "%u.%u.%u.%u", a & 255, a >> 8 & 255, a >> 16 & 255, a >> 24);
			snprintf(rh, sizeof(rh), "%u.%u.%u.%u", b & 255, b >> 8 & 255, b >> 16 & 255, b >> 24);
		} else {
			snprintf(lh, sizeof(lh), "[%.8s…]", la + 24);
			snprintf(rh, sizeof(rh), "[%.8s…]", ra + 24);
			if (!strncmp(la, "0000000000000000FFFF0000", 24)) {
				unsigned a = strtoul(la + 24, NULL, 16);
				snprintf(lh, sizeof(lh), "%u.%u.%u.%u", a & 255, a >> 8 & 255, a >> 16 & 255, a >> 24);
			}
			if (!strncmp(ra, "0000000000000000FFFF0000", 24)) {
				unsigned b = strtoul(ra + 24, NULL, 16);
				snprintf(rh, sizeof(rh), "%u.%u.%u.%u", b & 255, b >> 8 & 255, b >> 16 & 255, b >> 24);
			}
		}
		static const char *const tcp_st[] = { "", "established", "syn-sent", "syn-recv", "fin-wait1",
			"fin-wait2", "time-wait", "close", "close-wait", "last-ack", "listen", "closing" };
		struct sockinfo *more = realloc(sinfo, (n_sinfo + 1) * sizeof(*sinfo));
		if (!more) break;
		sinfo = more;
		struct sockinfo *s = &sinfo[n_sinfo++];
		s->ino = ino;
		if (rp == 0) {
			snprintf(s->what, sizeof(s->what), "%s %s:%u%s", proto, lh, lp,
				!strncmp(proto, "tcp", 3) && st == 10 ? " listening" : "");
		} else {
			snprintf(s->what, sizeof(s->what), "%s %s:%u → %s:%u%s%s", proto, lh, lp, rh, rp,
				!strncmp(proto, "tcp", 3) ? " " : "", !strncmp(proto, "tcp", 3) && st < 12 ? tcp_st[st] : "");
		}
	}
	fclose(f);
}

static void unix_table(void) {
	FILE *f = fopen("/proc/net/unix", "re");
	char line[512];
	if (!f) return;
	fgets(line, sizeof(line), f);
	while (fgets(line, sizeof(line), f)) {
		unsigned long ino;
		char path[300] = "";
		int n = sscanf(line, "%*s %*s %*s %*s %*s %*s %lu %299s", &ino, path);
		if (n < 1) continue;
		struct sockinfo *more = realloc(sinfo, (n_sinfo + 1) * sizeof(*sinfo));
		if (!more) break;
		sinfo = more;
		struct sockinfo *s = &sinfo[n_sinfo++];
		s->ino = ino;
		snprintf(s->what, sizeof(s->what), "unix %s", path[0] ? path : "(unnamed)");
	}
	fclose(f);
}

static void sock_tables(void) {
	free(sinfo);
	sinfo = NULL;
	n_sinfo = 0;
	sock_table("/proc/net/tcp", "tcp", false);
	sock_table("/proc/net/tcp6", "tcp6", true);
	sock_table("/proc/net/udp", "udp", false);
	sock_table("/proc/net/udp6", "udp6", true);
	unix_table();
}

static const char *sock_what(unsigned long ino) {
	for (int i = 0; i < n_sinfo; i++) if (sinfo[i].ino == ino) return sinfo[i].what;
	return NULL;
}

static int ofile_kind(const char *t) {
	if (t[0] == '/') {
		if (!strncmp(t, "/dev/", 5) && strncmp(t, "/dev/shm/", 9)) return 4;
		return 0;
	}
	if (!strncmp(t, "socket:", 7)) return 1;
	if (!strncmp(t, "pipe:", 5)) return 2;
	if (!strncmp(t, "anon_inode:", 11)) return 3;
	return 5;
}

static void collect_fds(pid_t pid, struct ofile **files, int *n, int *cap) {
	char dir[64];
	snprintf(dir, sizeof(dir), "/proc/%d/fd", pid);
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char path[96], t[256];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		ssize_t l = readlink(path, t, sizeof(t) - 1);
		if (l <= 0) continue;
		t[l] = 0;
		int found = -1;
		for (int i = 0; i < *n; i++) {
			if (!strcmp((*files)[i].target, t)) {
				found = i;
				break;
			}
		}
		if (found < 0) {
			if (*n == *cap) {
				*cap = *cap ? *cap * 2 : 256;
				struct ofile *more = realloc(*files, *cap * sizeof(**files));
				if (!more) break;
				*files = more;
			}
			found = (*n)++;
			struct ofile *o = &(*files)[found];
			memset(o, 0, sizeof(*o));
			snprintf(o->target, sizeof(o->target), "%s", t);
			o->kind = ofile_kind(t);
			if (o->kind == 1) {
				const char *w = sock_what(strtoul(t + 8, NULL, 10));
				snprintf(o->what, sizeof(o->what), "%s", w ? w : "");
			}
		}
		struct ofile *o = &(*files)[found];
		bool have = false;
		for (int k = 0; k < o->n && k < 8; k++) if (o->pids[k] == pid) have = true;
		if (!have) {
			if (o->n < 8) o->pids[o->n] = pid;
			o->n++;
		}
	}
	if (d) closedir(d);
}

static int ofile_cmp(const void *a, const void *b) {
	const struct ofile *x = a, *y = b;
	if (x->kind != y->kind) return x->kind - y->kind;
	if (x->n != y->n) return y->n - x->n;
	return strcmp(x->target, y->target);
}

static void jb_files(struct jb *j, struct ofile *files, int n) {
	qsort(files, n, sizeof(*files), ofile_cmp);
	jb_open(j, "files", '{');
	for (int k = 0; k < 6; k++) {
		jb_open(j, ofile_kinds[k], '[');
		for (int i = 0; i < n; i++) {
			if (files[i].kind != k) continue;
			jb_open(j, NULL, '{');
			jb_str(j, "path", files[i].target);
			if (files[i].what[0]) jb_str(j, "what", files[i].what);
			jb_int(j, "n", files[i].n);
			jb_open(j, "pids", '[');
			for (int p = 0; p < files[i].n && p < 8; p++) {
				jb_sep(j);
				jb_printf(j, "%d", files[i].pids[p]);
			}
			jb_close(j, ']');
			jb_close(j, '}');
		}
		jb_close(j, ']');
	}
	jb_close(j, '}');
	int counts[6] = { 0 };
	for (int i = 0; i < n; i++) counts[files[i].kind]++;
	jb_open(j, "file_counts", '{');
	for (int k = 0; k < 6; k++) jb_int(j, ofile_kinds[k], counts[k]);
	jb_close(j, '}');
}

static int find_group(const char *name) {
	for (int i = 0; i < n_groups; i++) if (!strcasecmp(groups[i].name, name)) return i;
	for (int i = 0; i < n_groups; i++) if (!strcasecmp(groups[i].id, name)) return i;
	for (int i = 0; i < n_groups; i++) if (strcasestr(groups[i].name, name)) return i;
	return -1;
}

static void answer_app(struct jb *j, const char *name, int window) {
	int gi = find_group(name);
	jb_open(j, NULL, '{');
	if (gi < 0 || groups[gi].kind == K_GAUGE) {
		jb_str(j, "error", "no such app");
		jb_close(j, '}');
		return;
	}
	const struct agroup *g = &groups[gi];
	jb_window_head(j, window);
	jb_str(j, "name", g->name);
	jb_str(j, "id", g->id);
	char kind[2] = { g->kind, 0 };
	jb_str(j, "kind", kind);
	jb_int(j, "live", g->live);
	struct tally t;
	group_window(g, window, &t);
	jb_tally(j, &t);
	jb_open(j, "since_charge", '{');
	jb_tally(j, &g->chg);
	jb_close(j, '}');
	jb_open(j, "hours", '[');
	int32_t h = hour_now();
	for (int k = HOURS - 1; k >= 0; k--) {
		int32_t want = h - k;
		int s = ((want % HOURS) + HOURS) % HOURS;
		jb_open(j, NULL, '{');
		jb_int(j, "hour", (long long)want * 3600);
		if (g->hour_at[s] == want) {
			jb_num(j, "wh", tally_e(&g->hour[s]) / 3600);
			jb_num(j, "cpu_s", g->hour[s].cpu_s);
			jb_num(j, "bytes", tally_bytes(&g->hour[s]));
		} else {
			jb_num(j, "wh", 0);
			jb_num(j, "cpu_s", 0);
			jb_num(j, "bytes", 0);
		}
		jb_close(j, '}');
	}
	jb_close(j, ']');
	jb_procs(j, gi, &t, true);
	jb_open(j, "exited_total", '{');
	jb_int(j, "n", g->n_exited);
	jb_tally(j, &g->exited_t);
	jb_close(j, '}');
	jb_open(j, "exited_procs", '[');
	for (int i = 0; i < EXITED_KEEP; i++) {
		if (!g->ex[i].comm[0]) continue;
		jb_open(j, NULL, '{');
		jb_str(j, "comm", g->ex[i].comm);
		jb_int(j, "n", g->ex[i].n);
		jb_tally(j, &g->ex[i].t);
		jb_close(j, '}');
	}
	jb_close(j, ']');
	struct ofile *files = NULL;
	int n = 0, cap = 0;
	sock_tables();
	for (int i = 0; i < n_procs; i++) {
		if (procs[i].g == gi && !procs[i].kthread) collect_fds(procs[i].pid, &files, &n, &cap);
	}
	jb_files(j, files, n);
	free(files);
	jb_close(j, '}');
}

static void kv_file(struct jb *j, const char *obj, const char *path, const char *const *keys, bool kb) {
	char buf[8192];
	read_text(path, buf, sizeof(buf));
	jb_open(j, obj, '{');
	for (int k = 0; keys[k]; k++) {
		char look[64];
		snprintf(look, sizeof(look), "\n%s:", keys[k]);
		char *s = strncmp(buf, look + 1, strlen(look + 1)) == 0 ? buf + strlen(look + 1) : strstr(buf, look);
		if (!s) continue;
		if (s != buf + strlen(look + 1)) s += strlen(look);
		while (*s == ' ' || *s == '\t') s++;
		if (kb) {
			jb_num(j, keys[k], strtod(s, NULL));
		} else {
			char v[128];
			snprintf(v, sizeof(v), "%.*s", (int)strcspn(s, "\n"), s);
			jb_str(j, keys[k], v);
		}
	}
	jb_close(j, '}');
}

static void answer_proc(struct jb *j, pid_t pid) {
	jb_open(j, NULL, '{');
	struct pstat s;
	if (!read_stat(pid, &s)) {
		jb_str(j, "error", "no such process");
		jb_close(j, '}');
		return;
	}
	struct proc *p = proc_find(pid);
	jb_int(j, "pid", pid);
	jb_int(j, "ppid", s.ppid);
	jb_str(j, "comm", s.comm);
	char st[2] = { s.state, 0 };
	jb_str(j, "state", st);
	if (p && p->g >= 0 && p->g < n_groups) {
		jb_str(j, "app", groups[p->g].name);
		jb_int(j, "app_root", p->root);
	}
	char buf[4096];
	int argc;
	read_cmdline(pid, buf, sizeof(buf), &argc, NULL);
	ssize_t len = 0;
	for (int i = 0; i < argc; i++) len += strlen(buf + len) + 1;
	for (ssize_t i = 0; i + 1 < len; i++) if (!buf[i]) buf[i] = ' ';
	jb_str(j, "cmdline", buf);
	read_exe(pid, buf, sizeof(buf));
	jb_str(j, "exe", buf);
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/cwd", pid);
	ssize_t l = readlink(path, buf, sizeof(buf) - 1);
	buf[l > 0 ? l : 0] = 0;
	jb_str(j, "cwd", buf);
	read_cgroup(pid, buf, sizeof(buf));
	jb_str(j, "cgroup", buf);
	jb_int(j, "uid", read_uid(pid));
	double up = 0;
	read_text("/proc/uptime", buf, sizeof(buf));
	up = strtod(buf, NULL) - s.start / hz;
	jb_num(j, "uptime_s", up);
	jb_num(j, "cpu_total_s", s.ticks / hz);	/* since it started, all threads */
	jb_int(j, "cpu_last", s.cpu);
	jb_int(j, "threads", s.threads);
	if (p) {
		jb_open(j, "tally", '{');
		jb_tally(j, &p->t);
		jb_close(j, '}');
	}
	static const char *const status_keys[] = { "VmPeak", "VmSize", "VmHWM", "VmRSS", "RssAnon", "RssFile",
		"RssShmem", "VmData", "VmSwap", "voluntary_ctxt_switches", "nonvoluntary_ctxt_switches", NULL };
	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	kv_file(j, "status_kb", path, status_keys, true);
	static const char *const smaps_keys[] = { "Rss", "Pss", "Pss_Anon", "Pss_File", "Pss_Shmem",
		"Shared_Clean", "Shared_Dirty", "Private_Clean", "Private_Dirty", "Swap", "SwapPss", NULL };
	snprintf(path, sizeof(path), "/proc/%d/smaps_rollup", pid);
	kv_file(j, "smaps_kb", path, smaps_keys, true);
	static const char *const io_keys[] = { "rchar", "wchar", "syscr", "syscw", "read_bytes", "write_bytes",
		"cancelled_write_bytes", NULL };
	snprintf(path, sizeof(path), "/proc/%d/io", pid);
	kv_file(j, "io", path, io_keys, true);
	/* the threads */
	jb_open(j, "thread_list", '[');
	snprintf(path, sizeof(path), "/proc/%d/task", pid);
	DIR *d = opendir(path);
	struct dirent *e;
	int shown = 0;
	while (d && (e = readdir(d)) && shown < 256) {
		pid_t tid = atoi(e->d_name);
		if (tid <= 0) continue;
		char tp[96];
		snprintf(tp, sizeof(tp), "/proc/%d/task/%d/stat", pid, tid);
		char tb[1024];
		if (read_text(tp, tb, sizeof(tb)) <= 0) continue;
		char *o = strchr(tb, '('), *c = strrchr(tb, ')');
		if (!o || !c) continue;
		unsigned long long ut = 0, stt = 0;
		char *f = c + 2;
		for (int i = 3; i < 14 && f; i++) f = strchr(f, ' ') ? strchr(f, ' ') + 1 : NULL;
		if (f) sscanf(f, "%llu %llu", &ut, &stt);
		char name[24];
		snprintf(name, sizeof(name), "%.*s", (int)(c - o - 1), o + 1);
		jb_open(j, NULL, '{');
		jb_int(j, "tid", tid);
		jb_str(j, "comm", name);
		jb_num(j, "cpu_s", (ut + stt) / hz);
		jb_close(j, '}');
		shown++;
	}
	if (d) closedir(d);
	jb_close(j, ']');
	struct ofile *files = NULL;
	int n = 0, cap = 0;
	sock_tables();
	collect_fds(pid, &files, &n, &cap);
	jb_files(j, files, n);
	free(files);
	jb_close(j, '}');
}

static double self_cpu_s(void) {
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

static void answer_status(struct jb *j) {
	jb_open(j, NULL, '{');
	jb_int(j, "pid", getpid());
	jb_int(j, "started", started_at);
	jb_num(j, "awake_s", now.mono_s);
	jb_num(j, "self_cpu_s", self_cpu_s());
	jb_int(j, "wakes", wakes);
	jb_int(j, "scans", scans);
	jb_int(j, "exits_seen", exits_seen);
	jb_int(j, "exit_overruns", exits_lost);
	jb_int(j, "taskstats", ts_fd >= 0);
	jb_int(j, "groups", n_groups);
	jb_int(j, "procs", n_procs);
	jb_int(j, "last_save", last_save);
	jb_int(j, "screen_on", now.screen_on);
	jb_int(j, "charging", now.charging);
	jb_int(j, "front_known", now.front_known);
	jb_int(j, "front_pid", now.front_pid);
	if (now.front_g >= 0) jb_str(j, "front", groups[now.front_g].name);
	char *mem = NULL;
	size_t ml = 0;
	FILE *mf = open_memstream(&mem, &ml);
	net_status(mf);
	energy_status(mf);
	fclose(mf);
	if (ml) {
		jb_sep(j);
		jb_raw(j, mem, ml);
	}
	free(mem);
	jb_close(j, '}');
}

/* ------------------------------------------------------------- socket */

static int listen_fd = -1;

static void serve_start(void) {
	mkdir(ACCT_RUN, 0750);
	chown(ACCT_RUN, 0, ztos_user_gid());
	chmod(ACCT_RUN, 0750);
	unlink(ACCT_SOCK);
	listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", ACCT_SOCK);
	if (listen_fd < 0 || bind(listen_fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(listen_fd, 8) < 0) {
		say("ztacctd: %s: %s", ACCT_SOCK, strerror(errno));
		return;
	}
	chown(ACCT_SOCK, 0, ztos_user_gid());
	chmod(ACCT_SOCK, 0660);
}

static void serve_one(void) {
	int c = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
	if (c < 0) return;
	struct ucred cr;
	socklen_t cl = sizeof(cr);
	if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &cl) < 0 || (cr.uid != 0 && cr.uid != ztos_user_uid())) {
		close(c);
		return;
	}
	struct timeval tv = { .tv_sec = 2 };
	setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	char line[512];
	size_t len = 0;
	while (len < sizeof(line) - 1) {
		ssize_t n = read(c, line + len, sizeof(line) - 1 - len);
		if (n <= 0) break;
		len += n;
		if (memchr(line, '\n', len)) break;
	}
	line[len] = 0;
	line[strcspn(line, "\r\n")] = 0;
	char orig[512];
	snprintf(orig, sizeof(orig), "%s", line);
	char *args[4] = { 0 };
	int na = 0;
	for (char *save = NULL, *t = strtok_r(line, " ", &save); t && na < 4; t = strtok_r(NULL, " ", &save)) args[na++] = t;
	struct jb j = { 0 };
	if (args[0] && !strcmp(args[0], "top")) {
		int w = parse_window(args[1]);
		int sort = args[2] ? (!strcmp(args[2], "cpu") ? 1 : !strcmp(args[2], "data") ? 2 :
			!strcmp(args[2], "bg") ? 3 : 0) : 0;
		if (w < 0) jb_printf(&j, "{\"error\":\"bad window\"}");
		else answer_top(&j, w, sort);
	} else if (args[0] && !strcmp(args[0], "app") && args[1]) {
		/* "app NAME WITH SPACES[@WINDOW]" */
		char *name = orig + 4;
		int w = 0;
		char *at = strrchr(name, '@');
		if (at) {
			*at = 0;
			w = parse_window(at + 1);
			if (w < 0) w = 0;
		}
		size_t nl = strlen(name);
		while (nl && name[nl - 1] == ' ') name[--nl] = 0;
		answer_app(&j, name, w);
	} else if (args[0] && !strcmp(args[0], "proc") && args[1]) {
		answer_proc(&j, atoi(args[1]));
	} else if (args[0] && !strcmp(args[0], "status")) {
		answer_status(&j);
	} else if (args[0] && !strcmp(args[0], "save")) {
		save_state();
		jb_printf(&j, "{\"saved\":%lld}", (long long)last_save);
	} else {
		jb_printf(&j, "{\"error\":\"ask: top [WINDOW [energy|cpu|data|bg]], app NAME[@WINDOW], proc PID, status, save\"}");
	}
	jb_raw(&j, "\n", 1);
	for (size_t off = 0; off < j.n;) {
		ssize_t w = write(c, j.p + off, j.n - off);
		if (w <= 0) break;
		off += w;
	}
	free(j.p);
	close(c);
}

static void write_status(void) {
	/* a few lines for anyone: whether it runs, and the totals since the charge */
	FILE *f = fopen(ACCT_STATUS ".new", "we");
	if (!f) return;
	struct tally *gt = &groups[g_gauge].chg;
	fprintf(f, "pid=%d\nupdated=%lld\nself_cpu_s=%.2f\ngroups=%d\nprocs=%d\nmeasured_wh=%.4f\nmodelled_wh=%.4f\nscreen=%s\nfront=%s\n",
		getpid(), (long long)now.wall, self_cpu_s(), n_groups, n_procs, gt->e[0] / 3600, gt->e[1] / 3600,
		now.screen_on ? "on" : "off", now.front_g >= 0 ? groups[now.front_g].name : "");
	fclose(f);
	chmod(ACCT_STATUS ".new", 0644);
	rename(ACCT_STATUS ".new", ACCT_STATUS);
}

/* --------------------------------------------------------------- main */

static void times_now(void) {
	now.boot_s = clock_s(CLOCK_BOOTTIME);
	now.mono_s = clock_s(CLOCK_MONOTONIC);
	now.wall = time(NULL);
}

int main(int argc, char **argv) {
	setvbuf(stderr, NULL, _IOLBF, 0);
	hz = sysconf(_SC_CLK_TCK);
	times_now();
	started_at = now.wall;
	read_text("/proc/sys/kernel/random/boot_id", boot_id, sizeof(boot_id));
	boot_id[strcspn(boot_id, "\n")] = 0;
	/* the special groups first: their places never change */
	g_gauge = group_get("__gauge", "gauge", K_GAUGE);
	g_kernel = group_get("Kernel", "kernel", K_KERNEL);
	g_shell = group_get("ztMETRO", "ztmetro", K_SHELL);
	g_short = group_get("Short-lived processes", "short", K_OTHER);
	g_unattr_net = group_get("Network: not attributed", "net", K_OTHER);
	now.front_g = g_shell;
	desktop_load();
	load_state();
	if (!charge_since) charge_since = now.wall;
	bool ts = taskstats_start();
	bool nt = net_start();
	energy_start();
	serve_start();
	find_shell();
	read_front();
	read_charger();
	scan();
	say("ztacctd: running (taskstats %s, network %s, shell %d, zygote %d)", ts ? "on" : "off",
		nt ? "on" : "off", shell_pid, zygote_pid);

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGHUP);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	signal(SIGPIPE, SIG_IGN);
	int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
	/* CLOCK_MONOTONIC stops while the SoC sleeps: we never wake it */
	int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	int ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (ino >= 0) inotify_add_watch(ino, "/run", IN_MOVED_TO);
	double last_scan_boot = now.boot_s, last_scan_mono = now.mono_s;
	last_save = now.wall;
	int interval = 0;
	for (;;) {
		int want = now.screen_on ? SCAN_ON_S : SCAN_OFF_S;
		if (want != interval) {
			interval = want;
			double left = last_scan_mono + interval - clock_s(CLOCK_MONOTONIC);
			if (left < 0.5) left = 0.5;
			struct itimerspec its = { .it_interval = { .tv_sec = interval },
				.it_value = { .tv_sec = (time_t)left, .tv_nsec = (long)((left - (time_t)left) * 1e9) } };
			timerfd_settime(tfd, 0, &its, NULL);
		}
		/* exit records wake us only with the screen on; off, they wait in the
		 * socket (4 MB) for the next scan, and an exiting process costs no wake */
		struct pollfd fds[5] = { { .fd = tfd, .events = POLLIN }, { .fd = now.screen_on ? ts_fd : -1, .events = POLLIN },
			{ .fd = listen_fd, .events = POLLIN }, { .fd = sfd, .events = POLLIN }, { .fd = ino, .events = POLLIN } };
		if (poll(fds, 5, -1) < 0) {
			if (errno == EINTR) continue;
			break;
		}
		wakes++;
		times_now();
		if (fds[1].revents) taskstats_read();
		if (fds[4].revents) {
			char ib[4096];
			bool ours = false;
			ssize_t n;
			while ((n = read(ino, ib, sizeof(ib))) > 0) {
				for (char *q = ib; q < ib + n;) {
					struct inotify_event *ev = (struct inotify_event *)q;
					if (ev->len && !strcmp(ev->name, "zrnperfd.status")) ours = true;
					q += sizeof(*ev) + ev->len;
				}
			}
			if (ours) read_front();
		}
		if (fds[3].revents) {
			struct signalfd_siginfo si;
			read(sfd, &si, sizeof(si));
			say("ztacctd: signal %d: saving", si.ssi_signo);
			taskstats_read();
			save_state();
			break;
		}
		if (fds[0].revents) {
			uint64_t exp;
			read(tfd, &exp, sizeof(exp));
			taskstats_read();	/* before /proc: what exited is in its records */
			read_charger();
			read_front();
			scan();
			net_tick(now.boot_s - last_scan_boot);
			energy_tick(now.boot_s - last_scan_boot, now.mono_s - last_scan_mono);
			last_scan_boot = now.boot_s;
			last_scan_mono = now.mono_s;
			if (now.wall - desks_read > 600) desktop_load();
			if (now.wall - last_save >= SAVE_S) save_state();
			write_status();
		}
		if (fds[2].revents) serve_one();
	}
	unlink(ACCT_SOCK);
	(void)argc;
	(void)argv;
	return 0;
}
