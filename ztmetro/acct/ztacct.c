// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztacct: battery and data use per app, from ztacctd.
 *
 *	ztacct [top] [-w WINDOW] [-s energy|cpu|data|bg] [-p] [-a] [--json]
 *		every app, sorted; -p shows each app's processes and their share,
 *		-a shows the ones with almost nothing too
 *	ztacct app NAME [-w WINDOW] [--json]
 *		one app: totals, foreground and background, data by network,
 *		its processes (live and exited) and their open files, merged
 *	ztacct proc PID [--json]
 *		one process: memory, threads, I/O, open files, CPU, data, energy
 *	ztacct status [--json]	the daemon: its own CPU time, the gauge, the model
 *	ztacct save		write the state file now
 *
 * WINDOW: charge (since the last full charge; the default), today, 24h, day:N.
 */
#define _GNU_SOURCE
#include <json-c/json.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "acct-common.h"

static const char *str(json_object *o, const char *k) {
	json_object *v;
	return json_object_object_get_ex(o, k, &v) ? json_object_get_string(v) : "";
}
static double num(json_object *o, const char *k) {
	json_object *v;
	return json_object_object_get_ex(o, k, &v) ? json_object_get_double(v) : 0;
}
static json_object *obj(json_object *o, const char *k) {
	json_object *v;
	return json_object_object_get_ex(o, k, &v) ? v : NULL;
}

static const char *bytes(double b, char *buf) {
	static char own[8][32];
	static int at;
	if (!buf) buf = own[at++ % 8];
	if (b < 1024) sprintf(buf, "%.0f B", b);
	else if (b < 1024 * 1024) sprintf(buf, "%.1f KB", b / 1024);
	else if (b < 1024.0 * 1024 * 1024) sprintf(buf, "%.1f MB", b / 1048576);
	else sprintf(buf, "%.2f GB", b / 1073741824.0);
	return buf;
}

static const char *dur(double s) {
	static char own[8][32];
	static int at;
	char *buf = own[at++ % 8];
	if (s < 60) sprintf(buf, "%.1f s", s);
	else if (s < 3600) sprintf(buf, "%dm%02ds", (int)s / 60, (int)s % 60);
	else sprintf(buf, "%dh%02dm", (int)s / 3600, (int)s / 60 % 60);
	return buf;
}

static const char *wh(double w) {
	static char own[8][32];
	static int at;
	char *buf = own[at++ % 8];
	if (w < 1) sprintf(buf, "%.1f mWh", w * 1000);
	else sprintf(buf, "%.2f Wh", w);
	return buf;
}

static double sum_net(json_object *t, const char *dir) {
	json_object *d = obj(t, dir);
	return d ? num(d, "wifi") + num(d, "mobile") + num(d, "other") : 0;
}

static const char *kind_name(const char *k) {
	switch (k[0]) {
	case 'a': return "app";
	case 'A': return "android";
	case 's': return "system";
	case 'u': return "service";
	case 'S': return "shell";
	case 'k': return "kernel";
	case 'l': return "login";
	case 'm': return "phone";
	default: return "other";
	}
}

static void head(json_object *r) {
	double meas = num(r, "measured_wh"), model = num(r, "modelled_wh");
	time_t since = (time_t)num(r, "charge_since");
	char when[64];
	strftime(when, sizeof(when), "%a %d %b %H:%M", localtime(&since));
	printf("window: %s", str(r, "window"));
	if (!strcmp(str(r, "window"), "charge")) printf(" (since %s)", when);
	printf("   on battery %s, charging %s\n", dur(num(r, "on_battery_s")), dur(num(r, "charging_s")));
	printf("battery gauge: %s used, model %s", wh(meas), wh(model));
	if (meas > 0) printf(" (%.0f%% of it)", 100 * model / meas);
	printf("   screen %s, in front: %s%s\n\n", num(r, "screen_on") ? "on" : "off", str(r, "front"),
		num(r, "front_known") ? "" : " (the shell does not say)");
}

static int top(json_object *r, bool procs, bool all) {
	head(r);
	json_object *gs = obj(r, "groups");
	printf("%-34s %-8s %10s %10s %9s %9s %9s %8s\n", "app", "kind", "energy", "cpu", "in front", "bg cpu",
		"received", "sent");
	int hidden = 0;
	for (size_t i = 0; gs && i < json_object_array_length(gs); i++) {
		json_object *g = json_object_array_get_idx(gs, i);
		double e = num(g, "wh"), cpu = num(g, "cpu_s"), rx = sum_net(g, "rx"), tx = sum_net(g, "tx");
		if (!all && e < 0.00005 && cpu < 0.5 && rx + tx < 1024) {
			hidden++;
			continue;
		}
		char name[64];
		int live = (int)num(g, "live");
		snprintf(name, sizeof(name), "%s%s", str(g, "name"), live ? "" : " (gone)");
		printf("%-34.34s %-8s %10s %10s %9s %9s %9s %8s\n", name, kind_name(str(g, "kind")), wh(e), dur(cpu),
			dur(num(g, "fg_s")), dur(num(g, "bg_cpu_s")), bytes(rx, NULL), bytes(tx, NULL));
		json_object *ps = obj(g, "procs");
		for (size_t k = 0; procs && ps && k < json_object_array_length(ps); k++) {
			json_object *p = json_object_array_get_idx(ps, k);
			char pn[64];
			snprintf(pn, sizeof(pn), "  %d %s", (int)num(p, "pid"), str(p, "comm"));
			printf("%-34.34s %7.0f%% %10s %10s %9s %9s %9s %8s\n", pn, 100 * num(p, "share_cpu"), wh(num(p, "wh")),
				dur(num(p, "cpu_s")), "", dur(num(p, "bg_cpu_s")), bytes(sum_net(p, "rx"), NULL),
				bytes(sum_net(p, "tx"), NULL));
		}
	}
	if (hidden) printf("(%d more with almost nothing: -a shows them)\n", hidden);
	return 0;
}

static void net_line(json_object *t) {
	json_object *rx = obj(t, "rx"), *tx = obj(t, "tx");
	printf("  data         Wi-Fi %s in, %s out; mobile %s in, %s out; other %s in, %s out\n",
		bytes(num(rx, "wifi"), NULL), bytes(num(tx, "wifi"), NULL), bytes(num(rx, "mobile"), NULL),
		bytes(num(tx, "mobile"), NULL), bytes(num(rx, "other"), NULL), bytes(num(tx, "other"), NULL));
}

static void energy_line(json_object *t) {
	json_object *e = obj(t, "e_wh");
	printf("  energy       %s: CPU %s, network %s, screen %s, GPU %s, base %s\n", wh(num(t, "wh")),
		wh(num(e, "cpu")), wh(num(e, "net")), wh(num(e, "screen")), wh(num(e, "gpu")), wh(num(e, "base")));
}

static void files(json_object *r) {
	json_object *f = obj(r, "files"), *c = obj(r, "file_counts");
	static const char *const kinds[] = { "files", "sockets", "pipes", "anon", "devices", "other" };
	static const char *const titles[] = { "files", "sockets", "pipes", "anonymous inodes", "devices", "other" };
	for (int k = 0; k < 6; k++) {
		json_object *l = obj(f, kinds[k]);
		if (!l || !json_object_array_length(l)) continue;
		printf("\n%s (%d)\n", titles[k], (int)num(c, kinds[k]));
		for (size_t i = 0; i < json_object_array_length(l); i++) {
			json_object *o = json_object_array_get_idx(l, i);
			json_object *pids = obj(o, "pids");
			char who[96] = "";
			int n = (int)num(o, "n"), len = 0;
			for (size_t p = 0; pids && p < json_object_array_length(pids) && len < 80; p++) {
				len += snprintf(who + len, sizeof(who) - len, "%s%d", p ? "," : "",
					json_object_get_int(json_object_array_get_idx(pids, p)));
			}
			if (n > 8) snprintf(who + len, sizeof(who) - len, ",…");
			const char *what = str(o, "what");
			printf("  %-3d %-60s %s%s%s\n", n, str(o, "path"), what[0] ? what : "", what[0] ? "  " : "", who);
		}
	}
}

static int app(json_object *r) {
	if (str(r, "error")[0]) {
		fprintf(stderr, "ztacct: %s\n", str(r, "error"));
		return 1;
	}
	head(r);
	printf("%s  (%s, %s)\n", str(r, "name"), kind_name(str(r, "kind")), str(r, "id"));
	printf("  CPU          %s, %s of it in the background\n", dur(num(r, "cpu_s")), dur(num(r, "bg_cpu_s")));
	printf("  in front     %s with the screen on\n", dur(num(r, "fg_s")));
	net_line(r);
	energy_line(r);
	json_object *ps = obj(r, "procs");
	printf("\nprocesses now (%d)\n", ps ? (int)json_object_array_length(ps) : 0);
	for (size_t k = 0; ps && k < json_object_array_length(ps); k++) {
		json_object *p = json_object_array_get_idx(ps, k);
		printf("  %-7d %-16s %5.1f%% cpu %9s %9s  in %9s out %9s  %.60s\n", (int)num(p, "pid"), str(p, "comm"),
			100 * num(p, "share_cpu"), dur(num(p, "cpu_s")), wh(num(p, "wh")), bytes(sum_net(p, "rx"), NULL),
			bytes(sum_net(p, "tx"), NULL), str(p, "cmdline"));
	}
	json_object *et = obj(r, "exited_total"), *ex = obj(r, "exited_procs");
	if (et && num(et, "n") > 0) {
		printf("\nexited since the charge (%d): %s CPU, %s\n", (int)num(et, "n"), dur(num(et, "cpu_s")), wh(num(et, "wh")));
		for (size_t k = 0; ex && k < json_object_array_length(ex); k++) {
			json_object *p = json_object_array_get_idx(ex, k);
			printf("  %-20s x%-5d %9s %9s  data %s\n", str(p, "comm"), (int)num(p, "n"), dur(num(p, "cpu_s")),
				wh(num(p, "wh")), bytes(sum_net(p, "rx") + sum_net(p, "tx"), NULL));
		}
	}
	files(r);
	return 0;
}

static int proc(json_object *r) {
	if (str(r, "error")[0]) {
		fprintf(stderr, "ztacct: %s\n", str(r, "error"));
		return 1;
	}
	printf("%d %s  (%s)  state %s, parent %d, uid %d\n", (int)num(r, "pid"), str(r, "comm"), str(r, "app"),
		str(r, "state"), (int)num(r, "ppid"), (int)num(r, "uid"));
	printf("  command      %s\n  program      %s\n  in           %s\n  cgroup       %s\n", str(r, "cmdline"),
		str(r, "exe"), str(r, "cwd"), str(r, "cgroup"));
	printf("  running for  %s, %d threads, last on CPU %d\n", dur(num(r, "uptime_s")), (int)num(r, "threads"),
		(int)num(r, "cpu_last"));
	printf("  CPU          %s since it started", dur(num(r, "cpu_total_s")));
	json_object *t = obj(r, "tally");
	if (t) {
		printf(", %s counted (%s in the background)\n", dur(num(t, "cpu_s")), dur(num(t, "bg_cpu_s")));
		net_line(t);
		energy_line(t);
	} else {
		printf("\n");
	}
	json_object *st = obj(r, "status_kb"), *sm = obj(r, "smaps_kb"), *io = obj(r, "io");
	printf("  memory       RSS %s (anon %s, file %s, shmem %s), PSS %s, swap %s, peak RSS %s\n",
		bytes(num(st, "VmRSS") * 1024, NULL), bytes(num(st, "RssAnon") * 1024, NULL),
		bytes(num(st, "RssFile") * 1024, NULL), bytes(num(st, "RssShmem") * 1024, NULL),
		bytes(num(sm, "Pss") * 1024, NULL), bytes(num(st, "VmSwap") * 1024, NULL), bytes(num(st, "VmHWM") * 1024, NULL));
	printf("  I/O          read %s (%s from storage), written %s (%s to storage)\n", bytes(num(io, "rchar"), NULL),
		bytes(num(io, "read_bytes"), NULL), bytes(num(io, "wchar"), NULL), bytes(num(io, "write_bytes"), NULL));
	printf("  switches     %.0f voluntary, %.0f forced\n", num(st, "voluntary_ctxt_switches"),
		num(st, "nonvoluntary_ctxt_switches"));
	json_object *th = obj(r, "thread_list");
	if (th && json_object_array_length(th) > 1) {
		printf("\nthreads (%d)\n", (int)json_object_array_length(th));
		for (size_t i = 0; i < json_object_array_length(th); i++) {
			json_object *o = json_object_array_get_idx(th, i);
			printf("  %-7d %-16s %s\n", (int)num(o, "tid"), str(o, "comm"), dur(num(o, "cpu_s")));
		}
	}
	files(r);
	return 0;
}

static int status(json_object *r) {
	printf("ztacctd %d: %s CPU of its own in %s awake (%.3f%%), %d wakes, %d scans\n", (int)num(r, "pid"),
		dur(num(r, "self_cpu_s")), dur(num(r, "awake_s")), 100 * num(r, "self_cpu_s") / fmax(1, num(r, "awake_s")),
		(int)num(r, "wakes"), (int)num(r, "scans"));
	printf("processes %d, apps %d; exits seen %d (taskstats %s, overruns %d)\n", (int)num(r, "procs"),
		(int)num(r, "groups"), (int)num(r, "exits_seen"), num(r, "taskstats") ? "on" : "off",
		(int)num(r, "exit_overruns"));
	printf("%s\n", json_object_to_json_string_ext(r, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE));
	return 0;
}

static void usage(void) {
	fprintf(stderr, "usage: ztacct [top] [-w WINDOW] [-s energy|cpu|data|bg] [-p] [-a] [--json]\n"
		"       ztacct app NAME [-w WINDOW] [--json]\n"
		"       ztacct proc PID [--json]\n"
		"       ztacct status [--json] | save\n"
		"WINDOW: charge (default), today, 24h, day:N\n");
}

int main(int argc, char **argv) {
	const char *verb = "top", *window = "charge", *sort = "energy";
	char what[160] = "";
	bool json = false, procs = false, all = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--json") || !strcmp(argv[i], "-j")) json = true;
		else if (!strcmp(argv[i], "-p")) procs = true;
		else if (!strcmp(argv[i], "-a")) all = true;
		else if (!strcmp(argv[i], "-w") && i + 1 < argc) window = argv[++i];
		else if (!strcmp(argv[i], "-s") && i + 1 < argc) sort = argv[++i];
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
		else if (i == 1 && (!strcmp(argv[i], "top") || !strcmp(argv[i], "app") || !strcmp(argv[i], "proc") ||
				!strcmp(argv[i], "status") || !strcmp(argv[i], "save"))) verb = argv[i];
		else if (strlen(what) + strlen(argv[i]) + 2 < sizeof(what)) {
			if (what[0]) strcat(what, " ");
			strcat(what, argv[i]);
		}
	}
	char req[256];
	if (!strcmp(verb, "top")) snprintf(req, sizeof(req), "top %s %s", window, sort);
	else if (!strcmp(verb, "app") && what[0]) snprintf(req, sizeof(req), "app %s@%s", what, window);
	else if (!strcmp(verb, "proc") && what[0]) snprintf(req, sizeof(req), "proc %s", what);
	else if (!strcmp(verb, "status") || !strcmp(verb, "save")) snprintf(req, sizeof(req), "%s", verb);
	else { usage(); return 2; }
	char *answer = acct_ask(req);
	if (!answer) {
		fprintf(stderr, "ztacct: ztacctd does not answer at %s (is ztacctd.service running? are you its user?)\n", ACCT_SOCK);
		return 1;
	}
	if (json) {
		fputs(answer, stdout);
		return 0;
	}
	json_object *r = json_tokener_parse(answer);
	if (!r) {
		fprintf(stderr, "ztacct: could not read the answer\n");
		return 1;
	}
	if (str(r, "error")[0] && strcmp(verb, "app") && strcmp(verb, "proc")) {
		fprintf(stderr, "ztacct: %s\n", str(r, "error"));
		return 1;
	}
	int rc = !strcmp(verb, "top") ? top(r, procs, all) : !strcmp(verb, "app") ? app(r) :
		!strcmp(verb, "proc") ? proc(r) : !strcmp(verb, "status") ? status(r) : (printf("saved\n"), 0);
	json_object_put(r);
	free(answer);
	return rc;
}
