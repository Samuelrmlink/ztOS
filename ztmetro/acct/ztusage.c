// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * battery + data: what each app has used, from ztacctd (acct/README.md).
 *
 *   battery, data, cpu, background	the apps, by what they used most of it,
 *		since the last full charge, today, in the last 24 hours, yesterday
 *		or the day before (tap "Showing" to change). Tap an
 *		app: its processes open under it, each with its share; "details"
 *		goes to the app's page.
 *   an app	its totals, in front and in the background, data by network,
 *		where its energy went, its last 24 hours, its processes (now and
 *		exited) and the files they have open, merged: files first, then
 *		sockets, pipes, anonymous inodes and devices.
 *   a process	memory (RSS, PSS, swap), threads, I/O, open files, CPU, data,
 *		energy.
 *
 * The daemon is asked through /run/ztacct/sock (root or the phone's user).
 *
 *	ztusage --render battery|data|cpu|background|app:NAME|proc:PID FILE.png
 *		a picture of that page, with no display
 */
#define _GNU_SOURCE
#include <json-c/json.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "zrnmetrocore.h"
#include "acct-common.h"

enum page { PAGE_BATTERY, PAGE_DATA, PAGE_CPU, PAGE_BG, PAGE_APP, PAGE_PROC };
enum action { ACT_WINDOW = 1, ACT_ROW, ACT_DETAILS, ACT_PROC, ACT_MORE, ACT_FILES_MORE };

#define REFRESH_MS	10000
#define ROW_H		60
#define MAX_APPS	256
#define MAX_PROCS	48
#define GREY		0.6
#define RED		0xe51400

static const char *const windows[] = { "charge", "today", "24h", "day:1", "day:2" };
static const char *const window_titles[] = { "since the last full charge", "today", "the last 24 hours",
	"yesterday", "the day before yesterday" };
#define N_WINDOWS 5
static const char *const sorts[] = { "energy", "data", "cpu", "bg" };
static int window_at;

struct pr {
	int pid;
	char comm[24];
	double wh, cpu, share_cpu, share_wh, rx, tx;
};
struct app {
	char name[64], kind;
	double wh, cpu, fg, bg, rx[3], tx[3];
	int live, exited, np;
	struct pr procs[MAX_PROCS];
	bool open;
};
static struct app apps[MAX_APPS];
static int n_apps;
static double measured_wh, modelled_wh, on_battery_s, unaccounted_wh, explained = -1;
static char front[64];
static bool front_known, daemon_up, loaded;
static int list_page = PAGE_BATTERY;	/* which tab the list is for */
static bool show_all;

/* the rows of the list: an app, one of its processes, its details, or "more" */
struct row { int kind, a, p; };
static struct row rows[MAX_APPS * 4];
static int n_rows;
static uint64_t rows_rev;

static json_object *app_json, *proc_json;
static char app_name[64];
static int proc_pid;
static int files_limit = 30;

/* ------------------------------------------------------------- reading */

static const char *jstr(json_object *o, const char *k) {
	json_object *v;
	return o && json_object_object_get_ex(o, k, &v) ? json_object_get_string(v) : "";
}
static double jnum(json_object *o, const char *k) {
	json_object *v;
	return o && json_object_object_get_ex(o, k, &v) ? json_object_get_double(v) : 0;
}
static json_object *jobj(json_object *o, const char *k) {
	json_object *v;
	return o && json_object_object_get_ex(o, k, &v) ? v : NULL;
}
static double jnet(json_object *t, const char *dir) {
	json_object *d = jobj(t, dir);
	return jnum(d, "wifi") + jnum(d, "mobile") + jnum(d, "other");
}

static bool sorted_by_data(void) { return list_page == PAGE_DATA; }

static void build_rows(void) {
	zmc_list_lock(true);
	n_rows = 0;
	int hidden = 0;
	for (int i = 0; i < n_apps; i++) {
		struct app *a = &apps[i];
		bool tiny = a->wh < 0.00005 && a->cpu < 1 && a->rx[0] + a->rx[1] + a->rx[2] + a->tx[0] + a->tx[1] + a->tx[2] < 10240;
		if (sorted_by_data() && a->rx[0] + a->rx[1] + a->rx[2] + a->tx[0] + a->tx[1] + a->tx[2] < 1) tiny = true;
		if (tiny && !show_all) {
			hidden++;
			continue;
		}
		rows[n_rows++] = (struct row){ 0, i, -1 };
		if (a->open) {
			for (int p = 0; p < a->np; p++) rows[n_rows++] = (struct row){ 1, i, p };
			rows[n_rows++] = (struct row){ 2, i, -1 };
		}
	}
	if (hidden || show_all) rows[n_rows++] = (struct row){ 3, hidden, -1 };
	rows_rev++;
	zmc_list_lock(false);
}

static void load_list(void) {
	char req[64];
	int sort = list_page <= PAGE_BG ? list_page : 0;
	snprintf(req, sizeof(req), "top %s %s", windows[window_at], sorts[sort]);
	char *answer = acct_ask(req);
	loaded = true;
	daemon_up = answer != NULL;
	if (!answer) return;
	json_object *r = json_tokener_parse(answer);
	free(answer);
	if (!r) return;
	/* keep which apps were open */
	char open[16][64];
	int n_open = 0;
	for (int i = 0; i < n_apps && n_open < 16; i++) if (apps[i].open) snprintf(open[n_open++], 64, "%s", apps[i].name);
	measured_wh = jnum(r, "measured_wh");
	modelled_wh = jnum(r, "modelled_wh");
	on_battery_s = jnum(r, "on_battery_s");
	unaccounted_wh = jnum(r, "unaccounted_wh");
	explained = json_object_object_get_ex(r, "explained", NULL) ? jnum(r, "explained") : -1;
	snprintf(front, sizeof(front), "%s", jstr(r, "front"));
	front_known = jnum(r, "front_known") != 0;
	json_object *gs = jobj(r, "groups");
	zmc_list_lock(true);
	n_apps = 0;
	for (size_t i = 0; gs && i < json_object_array_length(gs) && n_apps < MAX_APPS; i++) {
		json_object *g = json_object_array_get_idx(gs, i);
		struct app *a = &apps[n_apps++];
		memset(a, 0, sizeof(*a));
		snprintf(a->name, sizeof(a->name), "%s", jstr(g, "name"));
		a->kind = jstr(g, "kind")[0];
		a->wh = jnum(g, "wh");
		a->cpu = jnum(g, "cpu_s");
		a->fg = jnum(g, "fg_s");
		a->bg = jnum(g, "bg_cpu_s");
		static const char *const nets[] = { "wifi", "mobile", "other" };
		for (int k = 0; k < 3; k++) {
			a->rx[k] = jnum(jobj(g, "rx"), nets[k]);
			a->tx[k] = jnum(jobj(g, "tx"), nets[k]);
		}
		a->live = (int)jnum(g, "live");
		a->exited = (int)jnum(g, "exited");
		json_object *ps = jobj(g, "procs");
		for (size_t k = 0; ps && k < json_object_array_length(ps) && a->np < MAX_PROCS; k++) {
			json_object *p = json_object_array_get_idx(ps, k);
			struct pr *q = &a->procs[a->np++];
			q->pid = (int)jnum(p, "pid");
			snprintf(q->comm, sizeof(q->comm), "%s", jstr(p, "comm"));
			q->wh = jnum(p, "wh");
			q->cpu = jnum(p, "cpu_s");
			q->share_cpu = jnum(p, "share_cpu");
			q->share_wh = jnum(p, "share_wh");
			q->rx = jnet(p, "rx");
			q->tx = jnet(p, "tx");
		}
		for (int k = 0; k < n_open; k++) if (!strcmp(open[k], a->name)) a->open = true;
	}
	zmc_list_lock(false);
	json_object_put(r);
	build_rows();
}

static void load_app(void) {
	char req[128];
	snprintf(req, sizeof(req), "app %s@%s", app_name, windows[window_at]);
	char *answer = acct_ask(req);
	if (app_json) json_object_put(app_json);
	app_json = answer ? json_tokener_parse(answer) : NULL;
	free(answer);
}

static void load_proc(void) {
	char req[64];
	snprintf(req, sizeof(req), "proc %d", proc_pid);
	char *answer = acct_ask(req);
	if (proc_json) json_object_put(proc_json);
	proc_json = answer ? json_tokener_parse(answer) : NULL;
	free(answer);
}

/* ------------------------------------------------------------ words */

static const char *bytes(double b) {
	static char own[16][24];
	static int at;
	char *s = own[at++ % 16];
	if (b < 1024) snprintf(s, 24, "%.0f B", b);
	else if (b < 1048576) snprintf(s, 24, "%.0f KB", b / 1024);
	else if (b < 1073741824.0) snprintf(s, 24, "%.1f MB", b / 1048576);
	else snprintf(s, 24, "%.2f GB", b / 1073741824.0);
	return s;
}
static const char *dur(double sec) {
	static char own[16][24];
	static int at;
	char *s = own[at++ % 16];
	if (sec < 60) snprintf(s, 24, "%.0f s", sec);
	else if (sec < 3600) snprintf(s, 24, "%d min", (int)(sec / 60 + 0.5));
	else snprintf(s, 24, "%d h %02d min", (int)sec / 3600, (int)sec / 60 % 60);
	return s;
}
static const char *wh(double w) {
	static char own[16][24];
	static int at;
	char *s = own[at++ % 16];
	if (w < 0.001) snprintf(s, 24, "%.2f mWh", w * 1000);
	else if (w < 1) snprintf(s, 24, "%.0f mWh", w * 1000);
	else snprintf(s, 24, "%.2f Wh", w);
	return s;
}

static double app_total(const struct app *a) {
	switch (list_page) {
	case PAGE_DATA: return a->rx[0] + a->rx[1] + a->rx[2] + a->tx[0] + a->tx[1] + a->tx[2];
	case PAGE_CPU: return a->cpu;
	case PAGE_BG: return a->bg;
	default: return a->wh;
	}
}

static double list_total(void) {
	double t = 0;
	for (int i = 0; i < n_apps; i++) t += app_total(&apps[i]);
	return t;
}

/* ------------------------------------------------------------- the list */

static void bar(cairo_t *cr, double x, double y, double w, double frac) {
	cairo_set_source_rgb(cr, 0.22, 0.22, 0.22);
	cairo_rectangle(cr, x, y, w, 4);
	cairo_fill(cr);
	if (frac > 0) {
		zmc_accent(cr);
		cairo_rectangle(cr, x, y, fmax(2, w * fmin(1, frac)), 4);
		cairo_fill(cr);
	}
}

static void app_detail(const struct app *a, char *out, size_t size) {
	double rx = a->rx[0] + a->rx[1] + a->rx[2], tx = a->tx[0] + a->tx[1] + a->tx[2];
	switch (list_page) {
	case PAGE_DATA:
		snprintf(out, size, "Wi-Fi %s · mobile %s · in %s, out %s", bytes(a->rx[0] + a->tx[0]),
			bytes(a->rx[1] + a->tx[1]), bytes(rx), bytes(tx));
		break;
	case PAGE_CPU:
		snprintf(out, size, "%s on the CPU · %s in front · %s", dur(a->cpu), dur(a->fg), wh(a->wh));
		break;
	case PAGE_BG:
		snprintf(out, size, "%s in the background · %s in all · %s", dur(a->bg), dur(a->cpu), wh(a->wh));
		break;
	default: {
		/* only what it has: the phone's own lines (idle, Wi-Fi idle) have no CPU or data */
		int l = snprintf(out, size, "%s", wh(a->wh));
		if (a->cpu >= 0.5) l += snprintf(out + l, size - l, " · CPU %s", dur(a->cpu));
		if (a->fg >= 0.5) l += snprintf(out + l, size - l, " · in front %s", dur(a->fg));
		if (rx + tx > 0) snprintf(out + l, size - l, " · %s", bytes(rx + tx));
		if (a->kind == 'm' && !strcmp(a->name, "Unaccounted"))
			snprintf(out, size, "%s the gauge measured beyond the model", wh(a->wh));
		else if (a->kind == 'm' && !strcmp(a->name, "USB-C accessories"))
			snprintf(out, size, "%s · the dock and what is plugged into it, on the phone's power", wh(a->wh));
		else if (a->kind == 'm' && !strcmp(a->name, "Desktop (external display)"))
			snprintf(out, size, "%s · the DisplayPort link and video out", wh(a->wh));
		else if (a->kind == 'm' && !strcmp(a->name, "Always-on (unseparated)"))
			snprintf(out, size, "%s · drawn whenever awake, not yet told apart", wh(a->wh));
	}
	}
	if (!a->live && a->kind != 'm') {
		size_t l = strlen(out);
		snprintf(out + l, size - l, " · not running");
	}
}

static void draw_row(cairo_t *cr, int r) {
	const struct row *row = &rows[r];
	double w = zmc.width - 2 * ZMC_MARGIN;
	if (row->kind == 3) {
		zmc_accent(cr);
		char t[64];
		if (show_all) snprintf(t, sizeof(t), "show fewer");
		else snprintf(t, sizeof(t), "%d more with almost nothing", row->a);
		zmc_text(cr, t, FONT_SEMILIGHT, 17, ZMC_MARGIN, 18, 0);
		return;
	}
	const struct app *a = &apps[row->a];
	double total = list_total();
	if (row->kind == 0) {
		double v = app_total(a), pct = total > 0 ? 100 * v / total : 0;
		char name[96], detail[160], pc[16];
		snprintf(name, sizeof(name), "%s", a->name);
		snprintf(pc, sizeof(pc), pct >= 0.5 ? "%.0f%%" : pct > 0 ? "<1%%" : "", pct);
		cairo_set_source_rgb(cr, 1, 1, 1);
		double pw = zmc_text_width(pc, FONT_SEMILIGHT, 20);
		zmc_text_line(cr, name, FONT_SEMILIGHT, 21, ZMC_MARGIN, 4, w - pw - 12);
		zmc_text(cr, pc, FONT_SEMILIGHT, 20, ZMC_MARGIN + w - pw, 5, 0);
		app_detail(a, detail, sizeof(detail));
		cairo_set_source_rgb(cr, GREY, GREY, GREY);
		zmc_text_line(cr, detail, FONT_REGULAR, 12.5, ZMC_MARGIN, 33, w);
		bar(cr, ZMC_MARGIN, 52, w, total > 0 ? v / total : 0);
		return;
	}
	double x = ZMC_MARGIN + 24;
	if (row->kind == 2) {
		zmc_accent(cr);
		char t[96];
		snprintf(t, sizeof(t), "details, open files%s", a->exited ? ", exited processes" : "");
		zmc_text(cr, t, FONT_SEMILIGHT, 17, x, 16, 0);
		return;
	}
	const struct pr *p = &a->procs[row->p];
	double share = list_page == PAGE_BATTERY ? p->share_wh : p->share_cpu;
	if (list_page == PAGE_DATA) {
		double at = a->rx[0] + a->rx[1] + a->rx[2] + a->tx[0] + a->tx[1] + a->tx[2];
		share = at > 0 ? (p->rx + p->tx) / at : 0;
	}
	char name[64], detail[128];
	snprintf(name, sizeof(name), "%s", p->comm);
	snprintf(detail, sizeof(detail), "%d · %.0f%% · CPU %s · %s · in %s, out %s", p->pid, 100 * share, dur(p->cpu),
		wh(p->wh), bytes(p->rx), bytes(p->tx));
	cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
	zmc_text_line(cr, name, FONT_SEMILIGHT, 18, x, 6, zmc.width - x - ZMC_MARGIN);
	cairo_set_source_rgb(cr, GREY, GREY, GREY);
	zmc_text_line(cr, detail, FONT_REGULAR, 12, x, 32, zmc.width - x - ZMC_MARGIN);
	bar(cr, x, 51, zmc.width - x - ZMC_MARGIN, share);
}

static bool row_hit(int r, double x, double y, struct zmc_hit *hit) {
	const struct row *row = &rows[r];
	int action = row->kind == 0 ? ACT_ROW : row->kind == 1 ? ACT_PROC : row->kind == 2 ? ACT_DETAILS : ACT_MORE;
	*hit = (struct zmc_hit){ .x = 0, .y = 0, .w = zmc.width, .h = ROW_H - 4, .action = action, .arg = r };
	return true;
}

static double note(cairo_t *cr, double y, const char *text, double size) {
	cairo_set_source_rgb(cr, GREY, GREY, GREY);
	return y + zmc_text(cr, text, FONT_REGULAR, size, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 10;
}

static double draw_down(cairo_t *cr, double y) {
	zmc_set_rgb(cr, RED);
	y += zmc_text(cr, "ztacctd isn't answering", FONT_SEMILIGHT, 22, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 8;
	return note(cr, y, "The accountant runs as ztacctd.service; it starts with the phone. "
		"Only the phone's user may ask it.", 13.5);
}

static double draw_list(cairo_t *cr, int page) {
	static const struct zmc_tab tabs[] = { { "battery", PAGE_BATTERY }, { "data", PAGE_DATA },
		{ "cpu", PAGE_CPU }, { "background", PAGE_BG } };
	double y = zmc_pivot_wheel(cr, tabs, 4, page);
	if (!loaded) return note(cr, y, "asking ztacctd", 13.5) + 30;
	if (!daemon_up) return draw_down(cr, y) + 20;
	if (page != list_page) {
		/* the tab beside, drawn ahead of the swipe: a line, not the list */
		return note(cr, y, "…", 13.5);
	}
	y = zmc_choice(cr, y, "Showing", window_titles[window_at], ACT_WINDOW);
	char line[256];
	if (page == PAGE_BATTERY) {
		if (measured_wh > 0 && explained >= 0) {
			/* the share explained: what is not Unaccounted (a model above the
			 * gauge is scaled down to it, so this is at most 100 %) */
			snprintf(line, sizeof(line), "The battery gave %s in %s on battery; the model explains %.0f%% of it. "
				"%s is under Unaccounted.%s", wh(measured_wh), dur(on_battery_s), 100 * explained,
				wh(unaccounted_wh),
				front_known ? "" : " The screen goes to ztMETRO: zrnperfd does not say which app is in front.");
		} else if (measured_wh > 0) {
			snprintf(line, sizeof(line), "The battery gave %s in %s on battery; the model accounts for %s (%.0f%%). "
				"What it cannot place is under Unaccounted.%s", wh(measured_wh), dur(on_battery_s),
				wh(modelled_wh), 100 * modelled_wh / measured_wh,
				front_known ? "" : " The screen goes to ztMETRO: zrnperfd does not say which app is in front.");
		} else {
			snprintf(line, sizeof(line), "Nothing measured on battery yet in this window; energy is the model's.");
		}
		y = note(cr, y, line, 12.5);
	} else if (page == PAGE_DATA) {
		double wi = 0, mo = 0, ot = 0;
		for (int i = 0; i < n_apps; i++) {
			wi += apps[i].rx[0] + apps[i].tx[0];
			mo += apps[i].rx[1] + apps[i].tx[1];
			ot += apps[i].rx[2] + apps[i].tx[2];
		}
		snprintf(line, sizeof(line), "Wi-Fi %s, mobile %s, other %s (USB, tunnels). Counted per socket, as IP packets.",
			bytes(wi), bytes(mo), bytes(ot));
		y = note(cr, y, line, 12.5);
	} else if (page == PAGE_BG) {
		y = note(cr, y, "CPU time while the app was not in front, or with the screen off.", 12.5);
	}
	y += 4;
	struct zmc_list list = { .count = n_rows, .row_h = ROW_H, .revision = rows_rev + window_at * 1000003u + list_page * 7919u,
		.draw_row = draw_row, .row_hit = row_hit };
	y = zmc_list(cr, y, &list);
	return y + 24;
}

/* ------------------------------------------------------------- an app */

static double section(cairo_t *cr, double y, const char *name) {
	zmc_accent(cr);
	zmc_text(cr, name, FONT_SEMIBOLD, 13, ZMC_MARGIN, y + 10, 0);
	return y + 38;
}

static double pair(cairo_t *cr, double y, const char *k, const char *v) {
	cairo_set_source_rgb(cr, GREY, GREY, GREY);
	zmc_text(cr, k, FONT_REGULAR, 13, ZMC_MARGIN, y, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	double x = ZMC_MARGIN + 118;
	return y + fmax(22, zmc_text(cr, v, FONT_REGULAR, 13, x, y, zmc.width - x - ZMC_MARGIN) + 6);
}

static double tally_block(cairo_t *cr, double y, json_object *t, bool cpu_line) {
	char v[200];
	json_object *rx = jobj(t, "rx"), *tx = jobj(t, "tx"), *e = jobj(t, "e_wh");
	snprintf(v, sizeof(v), "%s", wh(jnum(t, "wh")));
	y = pair(cr, y, "energy", v);
	snprintf(v, sizeof(v), "CPU %s · network %s · screen %s · GPU %s%s%s", wh(jnum(e, "cpu")), wh(jnum(e, "net")),
		wh(jnum(e, "screen")), wh(jnum(e, "gpu")), jnum(e, "base") > 0 ? " · base " : "",
		jnum(e, "base") > 0 ? wh(jnum(e, "base")) : "");
	y = pair(cr, y, "", v);
	if (cpu_line) {
		snprintf(v, sizeof(v), "%s, %s of it in the background", dur(jnum(t, "cpu_s")), dur(jnum(t, "bg_cpu_s")));
		y = pair(cr, y, "on the CPU", v);
		snprintf(v, sizeof(v), "%s with the screen on", dur(jnum(t, "fg_s")));
		y = pair(cr, y, "in front", v);
	}
	snprintf(v, sizeof(v), "in %s, out %s", bytes(jnum(rx, "wifi")), bytes(jnum(tx, "wifi")));
	y = pair(cr, y, "Wi-Fi", v);
	snprintf(v, sizeof(v), "in %s, out %s", bytes(jnum(rx, "mobile")), bytes(jnum(tx, "mobile")));
	y = pair(cr, y, "mobile data", v);
	if (jnum(rx, "other") + jnum(tx, "other") > 0) {
		snprintf(v, sizeof(v), "in %s, out %s", bytes(jnum(rx, "other")), bytes(jnum(tx, "other")));
		y = pair(cr, y, "other", v);
	}
	return y;
}

static double hours_chart(cairo_t *cr, double y, json_object *hours) {
	int n = hours ? (int)json_object_array_length(hours) : 0;
	if (!n) return y;
	double top = 0;
	for (int i = 0; i < n; i++) top = fmax(top, jnum(json_object_array_get_idx(hours, i), "wh"));
	double w = zmc.width - 2 * ZMC_MARGIN, bw = w / n, h = 56;
	for (int i = 0; i < n; i++) {
		double v = jnum(json_object_array_get_idx(hours, i), "wh");
		double bh = top > 0 ? h * v / top : 0;
		cairo_set_source_rgb(cr, 0.22, 0.22, 0.22);
		cairo_rectangle(cr, ZMC_MARGIN + i * bw + 1, y, bw - 2, h);
		cairo_fill(cr);
		if (bh > 0) {
			zmc_accent(cr);
			cairo_rectangle(cr, ZMC_MARGIN + i * bw + 1, y + h - fmax(1, bh), bw - 2, fmax(1, bh));
			cairo_fill(cr);
		}
	}
	cairo_set_source_rgb(cr, GREY, GREY, GREY);
	char left[32];
	time_t t0 = (time_t)jnum(json_object_array_get_idx(hours, 0), "hour");
	strftime(left, sizeof(left), "%H:00", localtime(&t0));
	zmc_text(cr, left, FONT_REGULAR, 11, ZMC_MARGIN, y + h + 4, 0);
	const char *right = "now";
	zmc_text(cr, right, FONT_REGULAR, 11, ZMC_MARGIN + w - zmc_text_width(right, FONT_REGULAR, 11), y + h + 4, 0);
	char peak[48];
	snprintf(peak, sizeof(peak), "top %s an hour", wh(top));
	zmc_text_centred(cr, peak, FONT_REGULAR, 11, ZMC_MARGIN + w / 2, y + h + 4);
	return y + h + 28;
}

static double files_block(cairo_t *cr, double y, json_object *r) {
	json_object *f = jobj(r, "files"), *c = jobj(r, "file_counts");
	static const char *const kinds[] = { "files", "sockets", "pipes", "anon", "devices", "other" };
	static const char *const titles[] = { "open files", "sockets", "pipes", "anonymous inodes", "devices", "other" };
	bool any = false;
	for (int k = 0; k < 6; k++) {
		json_object *l = jobj(f, kinds[k]);
		int n = l ? (int)json_object_array_length(l) : 0;
		if (!n) continue;
		any = true;
		char title[64];
		snprintf(title, sizeof(title), "%s (%d)", titles[k], (int)jnum(c, kinds[k]));
		y = section(cr, y, title);
		for (int i = 0; i < n && i < files_limit; i++) {
			json_object *o = json_object_array_get_idx(l, i);
			const char *what = jstr(o, "what");
			cairo_set_source_rgb(cr, 1, 1, 1);
			y += zmc_text(cr, what[0] ? what : jstr(o, "path"), FONT_MONO, 11, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 2;
			json_object *pids = jobj(o, "pids");
			char who[160];
			int len = snprintf(who, sizeof(who), "%s%s", what[0] ? jstr(o, "path") : "", what[0] ? " · " : "");
			int held = (int)jnum(o, "n");
			len += snprintf(who + len, sizeof(who) - len, held == 1 ? "process " : "%d processes: ", held);
			for (size_t p = 0; pids && p < json_object_array_length(pids) && len < 140; p++)
				len += snprintf(who + len, sizeof(who) - len, "%s%d", p ? ", " : "",
					json_object_get_int(json_object_array_get_idx(pids, p)));
			if (held > 8) snprintf(who + len, sizeof(who) - len, ", …");
			cairo_set_source_rgb(cr, GREY, GREY, GREY);
			y += zmc_text(cr, who, FONT_REGULAR, 11, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 8;
		}
		if (n > files_limit) {
			char more[48];
			snprintf(more, sizeof(more), "%d more", n - files_limit);
			y = zmc_entry(cr, y, more, NULL, ACT_FILES_MORE, 0);
		}
	}
	if (!any) y = note(cr, y, "No open files: nothing of it is running.", 12.5);
	return y;
}

static double draw_app(cairo_t *cr) {
	json_object *r = app_json;
	double y = zmc_header(cr, r && !jobj(r, "error") ? jstr(r, "name") : app_name);
	if (!r || jobj(r, "error")) return note(cr, y, "ztacctd does not know this app now.", 13.5);
	char v[200];
	snprintf(v, sizeof(v), "%s, %s", window_titles[window_at], jnum(r, "live") ? "running" : "not running");
	y = note(cr, y, v, 12.5);
	y = tally_block(cr, y, r, true);
	y = section(cr, y, "the last 24 hours");
	y = hours_chart(cr, y, jobj(r, "hours"));
	json_object *ps = jobj(r, "procs");
	int np = ps ? (int)json_object_array_length(ps) : 0;
	snprintf(v, sizeof(v), "processes now (%d)", np);
	y = section(cr, y, v);
	for (int i = 0; i < np && i < 64; i++) {
		json_object *p = json_object_array_get_idx(ps, i);
		char t[96], d[200];
		snprintf(t, sizeof(t), "%s", jstr(p, "comm"));
		snprintf(d, sizeof(d), "%d · %.0f%% of its CPU · %s · %s · in %s, out %s", (int)jnum(p, "pid"),
			100 * jnum(p, "share_cpu"), dur(jnum(p, "cpu_s")), wh(jnum(p, "wh")), bytes(jnet(p, "rx")),
			bytes(jnet(p, "tx")));
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_line(cr, t, FONT_SEMILIGHT, 19, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN);
		cairo_set_source_rgb(cr, GREY, GREY, GREY);
		zmc_text_line(cr, d, FONT_REGULAR, 12, ZMC_MARGIN, y + 27, zmc.width - 2 * ZMC_MARGIN);
		zmc_add_hit(0, y - 2, zmc.width, 50, ACT_PROC, -1 - (int)jnum(p, "pid"));
		y += 52;
	}
	json_object *et = jobj(r, "exited_total"), *ex = jobj(r, "exited_procs");
	if (jnum(et, "n") > 0) {
		snprintf(v, sizeof(v), "exited since the charge (%d)", (int)jnum(et, "n"));
		y = section(cr, y, v);
		for (size_t i = 0; ex && i < json_object_array_length(ex); i++) {
			json_object *p = json_object_array_get_idx(ex, i);
			char t[96], d[160];
			snprintf(t, sizeof(t), "%s ×%d", jstr(p, "comm"), (int)jnum(p, "n"));
			snprintf(d, sizeof(d), "CPU %s · %s · data %s", dur(jnum(p, "cpu_s")), wh(jnum(p, "wh")),
				bytes(jnet(p, "rx") + jnet(p, "tx")));
			y = pair(cr, y, t, d);
		}
	}
	y = files_block(cr, y, r);
	return y + 24;
}

/* ---------------------------------------------------------- a process */

static double draw_proc(cairo_t *cr) {
	json_object *r = proc_json;
	char title[64];
	snprintf(title, sizeof(title), "%s", r && !jobj(r, "error") ? jstr(r, "comm") : "process");
	double y = zmc_header(cr, title);
	if (!r || jobj(r, "error")) return note(cr, y, "That process has exited.", 13.5);
	char v[400];
	snprintf(v, sizeof(v), "%d, of %s · running for %s", (int)jnum(r, "pid"), jstr(r, "app"), dur(jnum(r, "uptime_s")));
	y = note(cr, y, v, 12.5);
	json_object *st = jobj(r, "status_kb"), *sm = jobj(r, "smaps_kb"), *io = jobj(r, "io");
	y = section(cr, y, "memory");
	snprintf(v, sizeof(v), "%s (anonymous %s, files %s, shared %s)", bytes(jnum(st, "VmRSS") * 1024),
		bytes(jnum(st, "RssAnon") * 1024), bytes(jnum(st, "RssFile") * 1024), bytes(jnum(st, "RssShmem") * 1024));
	y = pair(cr, y, "resident", v);
	snprintf(v, sizeof(v), "%s (its share of shared pages)", bytes(jnum(sm, "Pss") * 1024));
	y = pair(cr, y, "proportional", v);
	snprintf(v, sizeof(v), "%s", bytes(jnum(st, "VmSwap") * 1024));
	y = pair(cr, y, "swapped", v);
	snprintf(v, sizeof(v), "%s", bytes(jnum(st, "VmHWM") * 1024));
	y = pair(cr, y, "most resident", v);
	y = section(cr, y, "use");
	snprintf(v, sizeof(v), "%s since it started, %d threads, last on CPU %d", dur(jnum(r, "cpu_total_s")),
		(int)jnum(r, "threads"), (int)jnum(r, "cpu_last"));
	y = pair(cr, y, "CPU", v);
	json_object *t = jobj(r, "tally");
	if (t) y = tally_block(cr, y, t, false);
	snprintf(v, sizeof(v), "read %s (%s from storage), wrote %s (%s to storage)", bytes(jnum(io, "rchar")),
		bytes(jnum(io, "read_bytes")), bytes(jnum(io, "wchar")), bytes(jnum(io, "write_bytes")));
	y = pair(cr, y, "I/O", v);
	snprintf(v, sizeof(v), "%.0f voluntary, %.0f forced", jnum(st, "voluntary_ctxt_switches"),
		jnum(st, "nonvoluntary_ctxt_switches"));
	y = pair(cr, y, "switches", v);
	y = section(cr, y, "what it is");
	y = pair(cr, y, "command", jstr(r, "cmdline"));
	y = pair(cr, y, "program", jstr(r, "exe"));
	y = pair(cr, y, "folder", jstr(r, "cwd"));
	y = pair(cr, y, "cgroup", jstr(r, "cgroup"));
	snprintf(v, sizeof(v), "%d, parent %d, state %s", (int)jnum(r, "uid"), (int)jnum(r, "ppid"), jstr(r, "state"));
	y = pair(cr, y, "user", v);
	json_object *th = jobj(r, "thread_list");
	int nt = th ? (int)json_object_array_length(th) : 0;
	if (nt > 1) {
		snprintf(v, sizeof(v), "threads (%d)", nt);
		y = section(cr, y, v);
		for (int i = 0; i < nt && i < 48; i++) {
			json_object *o = json_object_array_get_idx(th, i);
			char k[48];
			snprintf(k, sizeof(k), "%d", (int)jnum(o, "tid"));
			snprintf(v, sizeof(v), "%s · %s", jstr(o, "comm"), dur(jnum(o, "cpu_s")));
			y = pair(cr, y, k, v);
		}
	}
	y = files_block(cr, y, r);
	return y + 24;
}

/* --------------------------------------------------------------- the app */

static double render(cairo_t *cr, int page) {
	switch (page) {
	case PAGE_APP: return draw_app(cr);
	case PAGE_PROC: return draw_proc(cr);
	default: return draw_list(cr, page);
	}
}

static int came_from = PAGE_BATTERY;

static void activate(int action, int arg) {
	if (action == ACT_WINDOW) {
		window_at = (window_at + 1) % N_WINDOWS;
		load_list();
		zmc_turn(1);
	} else if (action == ACT_ROW && arg >= 0 && arg < n_rows) {
		struct app *a = &apps[rows[arg].a];
		a->open = !a->open;
		build_rows();
		zmc_redraw();
	} else if (action == ACT_DETAILS && arg >= 0 && arg < n_rows) {
		snprintf(app_name, sizeof(app_name), "%s", apps[rows[arg].a].name);
		files_limit = 30;
		load_app();
		came_from = PAGE_APP;
		zmc_go(PAGE_APP, 1);
	} else if (action == ACT_PROC) {
		if (arg >= 0 && arg < n_rows && rows[arg].kind == 1) {
			proc_pid = apps[rows[arg].a].procs[rows[arg].p].pid;
			came_from = list_page;
		} else if (arg < 0) {
			proc_pid = -1 - arg;
			came_from = PAGE_APP;
		}
		files_limit = 30;
		load_proc();
		zmc_go(PAGE_PROC, 1);
	} else if (action == ACT_MORE) {
		show_all = !show_all;
		build_rows();
		zmc_redraw();
	} else if (action == ACT_FILES_MORE) {
		files_limit += 200;
		zmc_redraw();
	}
}

static bool hold(int action, int arg) {
	/* held, an app goes straight to its page */
	if (action == ACT_ROW) {
		activate(ACT_DETAILS, arg);
		return true;
	}
	return false;
}

static bool back(void) {
	if (zmc.page == PAGE_PROC) {
		zmc_go(came_from == PAGE_APP ? PAGE_APP : list_page, -1);
		return true;
	}
	if (zmc.page == PAGE_APP) {
		came_from = list_page;
		zmc_go(list_page, -1);
		return true;
	}
	return false;
}

static void arrived(int page) {
	if (page <= PAGE_BG && page != list_page) {
		list_page = page;
		load_list();
		zmc_redraw();
		zmc_pivot_changed();
	}
}

static void tick(void) {
	if (zmc.page <= PAGE_BG) {
		load_list();
		zmc_redraw();
	}
}

static const struct zmc_app app = {
	.app_id = "ztusage", .title = "battery + data", .caps = "BATTERY + DATA",
	.render = render, .activate = activate, .back = back, .arrived = arrived, .hold = hold,
};

int main(int argc, char **argv) {
	if (argc == 4 && !strcmp(argv[1], "--render")) {
		int page = PAGE_BATTERY;
		const char *what = argv[2];
		if (!strcmp(what, "data")) page = PAGE_DATA;
		else if (!strcmp(what, "cpu")) page = PAGE_CPU;
		else if (!strcmp(what, "background")) page = PAGE_BG;
		list_page = page <= PAGE_BG ? page : PAGE_BATTERY;
		load_list();
		if (!strncmp(what, "open:", 5)) {
			/* the list with one app opened */
			for (int i = 0; i < n_apps; i++) if (!strcasecmp(apps[i].name, what + 5)) apps[i].open = true;
			build_rows();
		} else if (!strncmp(what, "app:", 4)) {
			snprintf(app_name, sizeof(app_name), "%s", what + 4);
			load_app();
			page = PAGE_APP;
		} else if (!strncmp(what, "proc:", 5)) {
			proc_pid = atoi(what + 5);
			load_proc();
			page = PAGE_PROC;
		}
		const char *h = getenv("ZTUSAGE_RENDER_H");
		return zmc_render_png(&app, page, h ? atoi(h) : 1400, argv[3]);
	}
	load_list();
	zmc_every(REFRESH_MS, tick);
	return zmc_run(&app, PAGE_BATTERY);
}
