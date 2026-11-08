// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztacctd: bytes sent and received per process, per network.
 *
 * Linux keeps no byte count per process. acct-bpf.h's programs count every
 * packet a socket sends or receives, by socket (its cookie) and interface,
 * and note which process made each socket. Here, at every scan, the counts
 * that have grown since the last are given to that process and its app:
 *
 *   - the interface says the network: wl* Wi-Fi, rmnet* (and ccmni*, wwan*)
 *     mobile data, anything else (USB, a tunnel) other; loopback is not
 *     counted at all. A socket in another network namespace (Waydroid's
 *     container) sees only its own virtual interface: its bytes go to the
 *     network the phone's default route is on.
 *   - sockets made before ztacctd started have no maker on record: at start,
 *     and later when such a socket moves bytes (at most every 10 minutes),
 *     every process's socket descriptors are looked at (pidfd_getfd, then
 *     SO_COOKIE), and each socket goes to the process holding it.
 *   - bytes of a socket whose maker has gone go to the app it belonged to;
 *     bytes of sockets nobody can be found for: "Network: not attributed".
 *
 * Counts are of IP packets (headers included), as the interface's own
 * counters, which net_status() sets beside the attributed totals.
 */
#include "acctd.h"
#include "acct-bpf.h"

#include <dirent.h>
#include <net/if.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>

#ifndef SO_NETNS_COOKIE
#define SO_NETNS_COOKIE 71
#endif
#ifndef SO_COOKIE
#define SO_COOKIE 57
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif

#define ENTRIES		16384
#define TABLE		32768
#define IDLE_TICKS	4	/* unchanged this many scans: read, then deleted */
#define RESCAN_S	600

static struct acct_bpf bpf;
static uint64_t host_netns;

struct seen {
	struct acct_skb_key k;
	struct acct_skb_val v;
	uint16_t idle;
	bool used;
};
static struct seen *tab[2];
static int cur;

static double unknown_since_rescan, attributed[NCLS][2], unattributed;
static double last_rescan = -1e9;
static unsigned rescans, entries, deleted;
static unsigned long long dev_base[NCLS][2], dev_now[NCLS][2];

bool net_up(void) { return bpf.up; }

static unsigned key_hash(const struct acct_skb_key *k) {
	uint64_t h = k->cookie * 0x9e3779b97f4a7c15ULL ^ ((uint64_t)k->ifindex << 1 | k->dir);
	return (unsigned)(h ^ h >> 29) % TABLE;
}

static struct seen *tab_find(struct seen *t, const struct acct_skb_key *k, bool add) {
	unsigned h = key_hash(k);
	for (int n = 0; n < TABLE; n++, h = (h + 1) % TABLE) {
		if (!t[h].used) {
			if (!add) return NULL;
			t[h].used = true;
			t[h].k = *k;
			return &t[h];
		}
		if (!memcmp(&t[h].k, k, sizeof(*k))) return &t[h];
	}
	return NULL;
}

/* ------------------------------------------------------- interfaces */

static struct { unsigned idx; int cls; } ifs[64];
static int n_ifs;

static int name_class(const char *name) {
	if (!strncmp(name, "wl", 2)) return NET_WIFI;
	if (!strncmp(name, "rmnet", 5) || !strncmp(name, "ccmni", 5) || !strncmp(name, "wwan", 4) ||
			!strncmp(name, "umts", 4))
		return NET_MOBILE;
	return NET_OTHER;
}

static int if_class(unsigned idx) {
	for (int i = 0; i < n_ifs; i++) if (ifs[i].idx == idx) return ifs[i].cls;
	char name[IF_NAMESIZE] = "";
	int cls = if_indextoname(idx, name) ? name_class(name) : NET_OTHER;
	if (n_ifs < 64) {
		ifs[n_ifs].idx = idx;
		ifs[n_ifs].cls = cls;
		n_ifs++;
	}
	return cls;
}

int net_default_class(void) {
	FILE *f = fopen("/proc/net/route", "re");
	char line[256], name[32];
	unsigned dest, flags;
	int cls = NET_WIFI, best = -1;
	unsigned metric, best_metric = ~0u;
	if (!f) return cls;
	fgets(line, sizeof(line), f);
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%31s %x %*x %x %*d %*d %u", name, &dest, &flags, &metric) != 4) continue;
		if (dest == 0 && (flags & 1) && metric < best_metric) {
			best_metric = metric;
			best = name_class(name);
		}
	}
	fclose(f);
	return best >= 0 ? best : cls;
}

static void dev_counters(unsigned long long out[NCLS][2]) {
	memset(out, 0, sizeof(unsigned long long) * NCLS * 2);
	FILE *f = fopen("/proc/net/dev", "re");
	char line[512];
	if (!f) return;
	while (fgets(line, sizeof(line), f)) {
		char *c = strchr(line, ':');
		if (!c) continue;
		*c = 0;
		char *name = line;
		while (*name == ' ') name++;
		if (!strcmp(name, "lo")) continue;
		unsigned long long rx, tx;
		if (sscanf(c + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &rx, &tx) != 2) continue;
		int cls = name_class(name);
		out[cls][0] += rx;
		out[cls][1] += tx;
	}
	fclose(f);
}

/* ------------------------------------------------- who holds a socket */

static void owners_rescan(void) {
	last_rescan = now.mono_s;
	rescans++;
	unknown_since_rescan = 0;
	DIR *d = opendir("/proc");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		pid_t pid = atoi(e->d_name);
		if (pid <= 0) continue;
		struct proc *p = proc_find(pid);
		if (p && p->kthread) continue;
		char dir[64];
		snprintf(dir, sizeof(dir), "/proc/%d/fd", pid);
		DIR *fd_dir = opendir(dir);
		if (!fd_dir) continue;
		int pidfd = -1;
		struct dirent *f;
		while ((f = readdir(fd_dir))) {
			if (f->d_name[0] == '.') continue;
			char path[96], target[64];
			snprintf(path, sizeof(path), "%s/%s", dir, f->d_name);
			ssize_t l = readlink(path, target, sizeof(target) - 1);
			if (l <= 0) continue;
			target[l] = 0;
			if (strncmp(target, "socket:", 7)) continue;
			if (pidfd < 0) pidfd = syscall(SYS_pidfd_open, pid, 0);
			if (pidfd < 0) break;
			int fd = syscall(SYS_pidfd_getfd, pidfd, atoi(f->d_name), 0);
			if (fd < 0) continue;
			int domain = 0;
			socklen_t len = sizeof(domain);
			uint64_t cookie = 0;
			socklen_t cl = sizeof(cookie);
			if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &len) == 0 &&
					(domain == AF_INET || domain == AF_INET6) &&
					getsockopt(fd, SOL_SOCKET, SO_COOKIE, &cookie, &cl) == 0 && cookie) {
				struct acct_owner o;
				if (acct_bpf_get(bpf.map_owners, &cookie, &o) != 0) {
					uint64_t ns = 0;
					socklen_t nl = sizeof(ns);
					getsockopt(fd, SOL_SOCKET, SO_NETNS_COOKIE, &ns, &nl);
					o = (struct acct_owner){ .tgid = (uint32_t)pid, .netns = ns };
					acct_bpf_put(bpf.map_owners, &cookie, &o);
				}
			}
			close(fd);
		}
		if (pidfd >= 0) close(pidfd);
		closedir(fd_dir);
	}
	if (d) closedir(d);
}

/* ------------------------------------------------------------- start */

bool net_start(void) {
	tab[0] = calloc(TABLE, sizeof(struct seen));
	tab[1] = calloc(TABLE, sizeof(struct seen));
	if (!tab[0] || !tab[1]) return false;
	if (!acct_bpf_start(&bpf, ENTRIES)) {
		fprintf(stderr, "ztacctd: network counters: %s\n", bpf.error);
		return false;
	}
	int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	socklen_t l = sizeof(host_netns);
	if (s >= 0) {
		getsockopt(s, SOL_SOCKET, SO_NETNS_COOKIE, &host_netns, &l);
		close(s);
	}
	owners_rescan();
	dev_counters(dev_base);
	memcpy(dev_now, dev_base, sizeof(dev_now));
	return true;
}

/* ------------------------------------------------------------ a scan */

void net_tick(double dt) {
	(void)dt;
	if (!bpf.up) return;
	struct seen *old = tab[cur], *fresh = tab[cur ^ 1];
	memset(fresh, 0, TABLE * sizeof(struct seen));
	struct acct_skb_key k, next, *prev = NULL;
	static struct acct_skb_key dead[ENTRIES];
	int n_dead = 0;
	int def_cls = -1;
	bool unknown_moved = false;
	entries = 0;
	while (acct_bpf_next(bpf.map_bytes, prev, &next) == 0) {
		k = next;
		prev = &k;
		struct acct_skb_val v;
		if (acct_bpf_get(bpf.map_bytes, &k, &v) != 0) continue;
		entries++;
		struct seen *was = tab_find(old, &k, false);
		double db = was ? (double)(v.bytes - was->v.bytes) : (double)v.bytes;
		double dp = was ? (double)(v.packets - was->v.packets) : (double)v.packets;
		if (was && v.bytes < was->v.bytes) {
			db = v.bytes;	/* deleted and made again */
			dp = v.packets;
		}
		struct seen *s = tab_find(fresh, &k, true);
		if (!s) continue;
		s->v = v;
		s->idle = db > 0 ? 0 : (was ? was->idle + 1 : 1);
		if (s->idle >= IDLE_TICKS && n_dead < ENTRIES) dead[n_dead++] = k;
		if (db <= 0) continue;
		/* whose, and on which network */
		struct acct_owner o = { 0 };
		bool owned = acct_bpf_get(bpf.map_owners, &k.cookie, &o) == 0;
		int cls;
		if (owned && host_netns && o.netns && o.netns != host_netns) {
			if (def_cls < 0) def_cls = net_default_class();
			cls = def_cls;
		} else {
			cls = if_class(k.ifindex);
		}
		int g = -1;
		struct proc *p = NULL;
		if (owned) {
			p = proc_find((pid_t)o.tgid);
			g = p ? p->g : group_of_pid((pid_t)o.tgid);
		}
		if (g < 0) {
			g = group_unattributed_net();
			unattributed += db;
			if (!owned) {
				unknown_moved = true;
				unknown_since_rescan += db;
			}
		}
		struct tally d = { 0 };
		if (k.dir) d.tx[cls] = db;
		else d.rx[cls] = db;
		group_add(g, &d);
		if (p) {
			if (k.dir) p->t.tx[cls] += db;
			else p->t.rx[cls] += db;
		}
		attributed[cls][k.dir ? 1 : 0] += db;
		energy_add_net(g, p, cls, db, dp);
	}
	/* read and quiet: out of the kernel's table (a new packet makes it again) */
	for (int i = 0; i < n_dead; i++) {
		acct_bpf_del(bpf.map_bytes, &dead[i]);
		struct seen *s = tab_find(fresh, &dead[i], false);
		if (s) s->idle = 0, s->v = (struct acct_skb_val){ 0 };
		deleted++;
	}
	cur ^= 1;
	if (unknown_moved && now.mono_s - last_rescan > RESCAN_S) owners_rescan();
	dev_counters(dev_now);
}

void net_status(FILE *f) {
	fprintf(f, "\"net\":{\"up\":%d,\"error\":\"%s\",\"entries\":%u,\"deleted\":%u,\"rescans\":%u,"
		"\"unattributed_bytes\":%.0f", bpf.up, bpf.error, entries, deleted, rescans, unattributed);
	/* what the interfaces counted since ztacctd started, beside what it gave to processes */
	for (int c = 0; c < NCLS; c++) {
		fprintf(f, ",\"%s\":{\"dev_rx\":%llu,\"dev_tx\":%llu,\"counted_rx\":%.0f,\"counted_tx\":%.0f}", net_names[c],
			dev_now[c][0] - dev_base[c][0], dev_now[c][1] - dev_base[c][1], attributed[c][0], attributed[c][1]);
	}
	fprintf(f, "},");
}
