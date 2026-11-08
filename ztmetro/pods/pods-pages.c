// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * AirPods' batteries, listening modes and settings, in the Windows Phone 8.1 look: the
 * pages of the ztPods app (ztpods.c) and of Settings' Bluetooth page (pods-pages.h).
 *
 * No Bluetooth in here. ztos-podsd has it all: the state comes from
 * $XDG_RUNTIME_DIR/ztos-pods-state.json, and each change goes to ztos-pods.sock as a line.
 * Four pivot pages: status, listening, controls, about. `ztpods MAC` opens on that pair.
 * What a row shows is what the daemon says that pair has, so other generations get fewer
 * rows, and a value the pods never report says so instead of guessing.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <json-c/json.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "zrnmetrocore.h"
#include "pods-pages.h"
#include "pods-art.h"
#include "pods-ui.h"
#include "zmc-gesture.h"

#define MARGIN ZMC_MARGIN
#define MAX_NAME 127

enum page { P_STATUS = PODS_UI_STATUS, P_LISTEN = PODS_UI_LISTEN, P_CONTROLS = PODS_UI_CONTROLS, P_ABOUT = PODS_UI_ABOUT };

enum {
	A_PICK = PODS_ACTIONS + 1, A_MODE, A_CYCLE, A_NAME, A_NAME_SAVE, A_FORGET, A_FORGET_YES, A_BLUETOOTH, A_CONNECT,
	A_ROW = PODS_ACTIONS + 100,	/* + which row of PODS_ROWS */
};

static int first_page;
static void (*open_bluetooth)(void);

static int page_of(int tab) { return first_page + tab; }

static double tab_header(cairo_t *cr, int tab) {
	const struct zmc_tab tabs[] = { { "status", page_of(P_STATUS) }, { "listening", page_of(P_LISTEN) },
		{ "controls", page_of(P_CONTROLS) }, { "about", page_of(P_ABOUT) } };
	return zmc_pivot_wheel(cr, tabs, 4, page_of(tab));
}

static json_object *root;	/* the whole state */
static json_object *dev;	/* the pair on show, inside root */
static char wanted[18];		/* which one that is */
static time_t state_mtime;
static long state_nsec;
static uint32_t last_local;	/* a change made here: the file is not read over it for a moment */
static int open_picker;
static bool editing, name_edited, forget_asked;
static char name_edit[MAX_NAME + 1];
static long fixture_now;	/* --render: drawn as of the file's own time, for stable pictures */

static void send_line(const char *line) {
	const char *rt = getenv("XDG_RUNTIME_DIR");
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof(a.sun_path), "%s/ztos-pods.sock", rt ? rt : "/tmp");
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return;
	if (sendto(fd, line, strlen(line), MSG_DONTWAIT, (struct sockaddr *)&a, sizeof(a)) < 0 && errno != ENOENT &&
			errno != ECONNREFUSED) {
		perror("ztpods: send");
	}
	close(fd);
}

static void send_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void send_fmt(const char *fmt, ...) {
	char line[300];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	send_line(line);
	last_local = zmc_now_ms();
}

/* ---- the state ---- */

static const char *js(json_object *o, const char *k, const char *d) {
	json_object *v;
	if (o && json_object_object_get_ex(o, k, &v) && json_object_is_type(v, json_type_string)) return json_object_get_string(v);
	return d;
}
static bool jb(json_object *o, const char *k) {
	json_object *v;
	return o && json_object_object_get_ex(o, k, &v) && json_object_get_boolean(v);
}
static long jl(json_object *o, const char *k, long d) {
	json_object *v;
	if (o && json_object_object_get_ex(o, k, &v) && json_object_is_type(v, json_type_int)) return (long)json_object_get_int64(v);
	return d;
}
static json_object *jo(json_object *o, const char *k) {
	json_object *v;
	return o && json_object_object_get_ex(o, k, &v) ? v : NULL;
}
static bool in_list(json_object *o, const char *k, const char *want) {
	json_object *a = jo(o, k);
	int n = a && json_object_is_type(a, json_type_array) ? (int)json_object_array_length(a) : 0;
	for (int i = 0; i < n; i++) {
		if (!strcmp(json_object_get_string(json_object_array_get_idx(a, i)), want)) return true;
	}
	return false;
}

static void pick_device(void) {
	json_object *list = jo(root, "devices");
	int n = list ? (int)json_object_array_length(list) : 0;
	dev = NULL;
	for (int i = 0; i < n; i++) {
		json_object *d = json_object_array_get_idx(list, i);
		if (!dev) dev = d;
		if (wanted[0] && !strcmp(js(d, "mac", ""), wanted)) dev = d;
	}
	if (dev && !wanted[0]) snprintf(wanted, sizeof(wanted), "%s", js(dev, "mac", ""));
}

static bool load_state(bool force) {
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char path[320];
	struct stat st;
	snprintf(path, sizeof(path), "%s/ztos-pods-state.json", rt ? rt : "/tmp");
	if (stat(path, &st) != 0) {
		if (!root && !force) return false;
		if (root) json_object_put(root);
		root = NULL;
		dev = NULL;
		return true;
	}
	if (!force && st.st_mtim.tv_sec == state_mtime && st.st_mtim.tv_nsec == state_nsec) return false;
	state_mtime = st.st_mtim.tv_sec;
	state_nsec = st.st_mtim.tv_nsec;
	json_object *fresh = json_object_from_file(path);
	if (!fresh) return false;
	if (root) json_object_put(root);
	root = fresh;
	pick_device();
	return true;
}

static bool ready(void) { return jb(dev, "ready"); }
static bool settings_open(void) { return ready() && jb(dev, "settings_known"); }

static int setting(const char *key) { return (int)jl(jo(dev, "settings"), key, -1); }

/* shown at once; the daemon's file catches up */
static void set_local(const char *key, int value) {
	json_object *s = jo(dev, "settings");
	if (s) json_object_object_add(s, key, json_object_new_int(value));
}

static void set_setting(const char *key, int value) {
	send_fmt("set %s %s %d", js(dev, "mac", ""), key, value);
	set_local(key, value);
}

static long now_s(void) { return fixture_now ? fixture_now : (long)time(NULL); }

static void age_text(char *out, size_t size, long seen) {
	pods_age_text(out, size, now_s() - seen);
}

/* ---- drawing ---- */

static double note(cairo_t *cr, double y, const char *s) {
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	return y + zmc_text(cr, s, FONT_REGULAR, 14, MARGIN, y, zmc.width - 2 * MARGIN) + 14;
}

static double section(cairo_t *cr, double y, const char *title) {
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, title, FONT_SEMIBOLD, 13, MARGIN, y, 0);
	return y + 26;
}

static struct pods_battery reading(json_object *b) {
	json_object *lv = jo(b, "level");
	struct pods_battery r = { false, 0, jb(b, "charging") };
	if (lv && json_object_is_type(lv, json_type_int)) {
		r.known = true;
		r.level = json_object_get_int(lv);
		if (r.level < 0) r.level = 0;
		if (r.level > 100) r.level = 100;
	}
	return r;
}

static void shape_path(cairo_t *cr, const struct pods_shape *sh, const struct pods_xf *t) {
	cairo_new_path(cr);
	if (sh->kind == PODS_S_PATH) {
		for (const struct pods_seg *g = sh->path; g->op != PODS_END; g++) {
			const float *q = g->p;
			switch (g->op) {
			case PODS_M: cairo_move_to(cr, pods_xf_x(t, q[0]), pods_xf_y(t, q[1])); break;
			case PODS_L: cairo_line_to(cr, pods_xf_x(t, q[0]), pods_xf_y(t, q[1])); break;
			case PODS_C:
				cairo_curve_to(cr, pods_xf_x(t, q[0]), pods_xf_y(t, q[1]), pods_xf_x(t, q[2]), pods_xf_y(t, q[3]),
					pods_xf_x(t, q[4]), pods_xf_y(t, q[5]));
				break;
			case PODS_Z: cairo_close_path(cr); break;
			}
		}
		return;
	}
	double x0 = pods_xf_x(t, sh->x), x1 = pods_xf_x(t, sh->x + sh->w), y = pods_xf_y(t, sh->y), h = sh->h * t->s;
	double x = x0 < x1 ? x0 : x1, w = fabs(x1 - x0);
	if (sh->kind == PODS_S_ELLIPSE) {
		cairo_save(cr);
		cairo_translate(cr, x + w / 2, y + h / 2);
		cairo_scale(cr, w / 2, h / 2);
		cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
		cairo_restore(cr);
	} else {
		double r = sh->r * t->s;
		cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
		cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
		cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
		cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
		cairo_close_path(cr);
	}
}

static void rgb_stop(cairo_pattern_t *p, double at, unsigned v) {
	cairo_pattern_add_color_stop_rgb(p, at, ((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0, (v & 255) / 255.0);
}

/* sets the source to one of PODS_FILLS; returns the pattern to free, if it made one */
static cairo_pattern_t *set_fill(cairo_t *cr, int which, const struct pods_xf *t) {
	const struct pods_fill *f = &PODS_FILLS[which];
	if (f->n == 1) {
		zmc_set_rgb(cr, f->rgb[0]);
		return NULL;
	}
	cairo_pattern_t *p = cairo_pattern_create_linear(pods_xf_x(t, f->x0), pods_xf_y(t, f->y0), pods_xf_x(t, f->x1), pods_xf_y(t, f->y1));
	for (int i = 0; i < f->n; i++) rgb_stop(p, f->at[i], f->rgb[i]);
	cairo_set_source(cr, p);
	return p;
}

static void draw_art(cairo_t *cr, const struct pods_shape *shapes, int count, const struct pods_xf *t, bool pro, bool lit) {
	/* outlines first, all of them, so where two shapes meet is no line */
	for (int i = 0; i < count; i++) {
		if (!shapes[i].edged || (shapes[i].pro_only && !pro)) continue;
		shape_path(cr, &shapes[i], t);
		zmc_set_rgb(cr, PODS_C_EDGE);
		cairo_set_line_width(cr, 1.6 * (t->s > 0.7f ? t->s : 0.7f));
		cairo_stroke(cr);
	}
	for (int i = 0; i < count; i++) {
		const struct pods_shape *sh = &shapes[i];
		if (sh->pro_only && !pro) continue;
		cairo_pattern_t *p = set_fill(cr, sh->fill == PODS_F_LED_OFF && lit ? PODS_F_LED_ON : sh->fill, t);
		if (sh->kind == PODS_S_LINE) {
			cairo_set_line_width(cr, sh->r * t->s);
			cairo_new_path(cr);
			cairo_move_to(cr, pods_xf_x(t, sh->x), pods_xf_y(t, sh->y));
			cairo_line_to(cr, pods_xf_x(t, sh->x + sh->w), pods_xf_y(t, sh->y + sh->h));
			cairo_stroke(cr);
		} else {
			shape_path(cr, sh, t);
			cairo_fill(cr);
		}
		if (p) cairo_pattern_destroy(p);
	}
}

/* The picture: the buds and the case, a ring for each battery (pods-art.h). */
static double draw_hero(cairo_t *cr, double y, const struct pods_ring *rings, int n, bool pro, bool lit) {
	struct pods_hero h;
	pods_hero_layout(&h, rings, n, zmc.width - 2 * MARGIN, 1.0f);
	cairo_save(cr);
	cairo_translate(cr, MARGIN, y + 6);
	for (int i = 0; i < h.n_art; i++) {
		struct pods_xf t = { h.art[i].x, h.art[i].y, h.art[i].s, h.art[i].art == PODS_ART_CASE ? PODS_CASE_W : PODS_BUD_W,
			h.art[i].art == PODS_ART_BUD_R };
		if (h.art[i].art == PODS_ART_CASE) draw_art(cr, PODS_CASE, PODS_N_CASE, &t, pro, lit);
		else draw_art(cr, PODS_BUD, PODS_N_BUD, &t, pro, lit);
	}
	double size = h.label_h * 0.8;
	for (int i = 0; i < h.n_rings; i++) {
		const struct pods_battery *b = &h.rings[i].ring.b;
		double cx = h.rings[i].cx, cy = h.rings[i].cy, r = h.rings[i].r;
		uint32_t colour = pods_battery_low(b) ? PODS_C_LOW : PODS_C_FULL;
		cairo_set_line_width(cr, r * PODS_RING_WIDTH * 2);
		cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
		cairo_new_path(cr);
		cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
		cairo_stroke(cr);
		if (b->known && b->level > 0) {
			zmc_set_rgb(cr, colour);
			cairo_set_line_cap(cr, b->level >= 100 ? CAIRO_LINE_CAP_BUTT : CAIRO_LINE_CAP_ROUND);
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, r, -M_PI / 2, -M_PI / 2 + 2 * M_PI * b->level / 100.0);
			cairo_stroke(cr);
			cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
		}
		if (b->charging) {
			zmc_set_rgb(cr, colour);
			cairo_new_path(cr);
			for (int k = 0; k < PODS_N_BOLT; k++) cairo_line_to(cr, cx + PODS_BOLT[k][0] * r, cy + PODS_BOLT[k][1] * r);
			cairo_close_path(cr);
			cairo_fill(cr);
		}
		/* the line under it: a badge, then the level */
		int parts = h.rings[i].ring.parts;
		char text[16];
		pods_ring_text(text, sizeof(text), &h.rings[i].ring);
		bool badge = parts == PODS_PART_LEFT || parts == PODS_PART_RIGHT || parts == PODS_PART_CASE;
		double bw = h.label_h * 0.78, gap = h.label_h * 0.25, tw = zmc_text_width(text, FONT_SEMILIGHT, size);
		double x = cx - (tw + (badge ? bw + gap : 0)) / 2, mid = h.rings[i].label_y + h.label_h / 2;
		if (parts == PODS_PART_CASE) {
			struct pods_shape c = { PODS_S_RRECT, 0, 0, 0, (float)x, (float)(mid - bw * 0.36), (float)bw, (float)(bw * 0.72), (float)(bw * 0.22), NULL };
			struct pods_xf one = { 0, 0, 1, 0, false };
			shape_path(cr, &c, &one);
			zmc_set_rgb(cr, PODS_C_BADGE);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, 0, 0, 0);
			cairo_set_line_width(cr, bw * 0.08 > 1 ? bw * 0.08 : 1);
			cairo_move_to(cr, x, c.y + c.h * 0.38);
			cairo_line_to(cr, x + bw, c.y + c.h * 0.38);
			cairo_stroke(cr);
		} else if (badge) {
			zmc_set_rgb(cr, PODS_C_BADGE);
			cairo_new_path(cr);
			cairo_arc(cr, x + bw / 2, mid, bw / 2, 0, 2 * M_PI);
			cairo_fill(cr);
			const char *letter = parts == PODS_PART_LEFT ? "L" : "R";
			double ls = bw * 0.62, lw = zmc_text_width(letter, FONT_SEMIBOLD, ls);
			cairo_set_source_rgb(cr, 0, 0, 0);
			zmc_text(cr, letter, FONT_SEMIBOLD, ls, x + (bw - lw) / 2, mid - ls * 0.68, 0);
		}
		if (badge) x += bw + gap;
		if (!b->known) cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		else if (pods_battery_low(b)) zmc_set_rgb(cr, colour);
		else cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, text, FONT_SEMILIGHT, size, x, mid - size * 0.68, 0);
	}
	cairo_restore(cr);
	return y + 6 + h.h + 14;
}

/* the batteries: the picture, then how old a reading is that didn't come over a link */
static double batteries(cairo_t *cr, double y, bool live) {
	json_object *bat = jo(dev, "battery");
	struct pods_ring rings[3];
	int n;
	if (jo(bat, "headset")) {
		rings[0].parts = PODS_PART_HEADSET;
		rings[0].b = reading(jo(bat, "headset"));
		n = 1;
	} else {
		struct pods_battery lb = reading(jo(bat, "left")), rb = reading(jo(bat, "right")), cb = reading(jo(bat, "case"));
		n = pods_rings(&lb, &rb, &cb, rings);
	}
	y = draw_hero(cr, y, rings, n, strstr(js(dev, "model_label", ""), "Pro") != NULL, !strcmp(js(dev, "lid", ""), "open"));
	struct { int parts; char age[32]; const char *from; } heard[3];
	int shown = 0;
	for (int i = 0; i < n; i++) {
		int p = rings[i].parts;
		json_object *b = jo(bat, p == PODS_PART_HEADSET ? "headset" : p & PODS_PART_LEFT ? "left" : p & PODS_PART_RIGHT ? "right" : "case");
		/* over a link too, the case says nothing when it is away: its reading has an age */
		const char *from = js(b, "source", "");
		bool fresh = p == PODS_PART_CASE ? live && (!strcmp(from, "aap") || !strcmp(from, "case")) && now_s() - jl(b, "seen", 0) < 300 : live;
		if (!rings[i].b.known || fresh) continue;
		heard[shown].parts = p;
		heard[shown].from = from;
		age_text(heard[shown++].age, sizeof(heard[0].age), jl(b, "seen", 0));
	}
	bool same = shown > 1;	/* all the same: said once */
	for (int i = 1; i < shown; i++) same &= !strcmp(heard[i].age, heard[0].age) && !strcmp(heard[i].from, heard[0].from);
	for (int i = 0; i < (same ? 1 : shown); i++) {
		char line[192];
		pods_heard_text(line, sizeof(line), same ? NULL : pods_ring_name(heard[i].parts), heard[i].age, heard[i].from);
		y = note(cr, y, line);
	}
	return y;
}

static double draw_nearby(cairo_t *cr, double y) {
	json_object *list = jo(root, "nearby");
	int n = list ? (int)json_object_array_length(list) : 0, shown = 0;
	for (int i = 0; i < n; i++) {
		json_object *b = json_object_array_get_idx(list, i);
		if (now_s() - jl(b, "seen", 0) > 30) continue;
		if (!shown++) {
			y = section(cr, y + 6, "NEARBY");
			y = note(cr, y, "Heard over the air. Anyone's AirPods look like this until they have connected "
				"to this phone once, and the levels are in steps of 10%.");
		}
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_line(cr, js(b, "model_label", "AirPods"), FONT_SEMILIGHT, 21, MARGIN, y, zmc.width - 2 * MARGIN);
		y += 30;
		json_object *bat = jo(b, "battery");
		char line[96] = "", part[32];
		static const char *const P[3][2] = { { "left", "L" }, { "right", "R" }, { "case", "case" } };
		for (int k = 0; k < 3; k++) {
			json_object *e = jo(bat, P[k][0]), *lv = jo(e, "level");
			if (!lv || !json_object_is_type(lv, json_type_int)) continue;
			snprintf(part, sizeof(part), "%s%s %d%%", line[0] ? ", " : "", P[k][1], json_object_get_int(lv));
			strncat(line, part, sizeof(line) - strlen(line) - 1);
		}
		y = note(cr, y, line[0] ? line : "No levels");
	}
	return y;
}

/* What a broadcast says of who has them. */
static const char *where_in_use(void) {
	static char text[96];
	pods_where_in_use(text, sizeof(text), js(dev, "host", "unknown"), jl(dev, "connection", -1), "this phone");
	return text;
}

/* Connecting them to this phone: BlueZ does it, and only for this tap. May take them from
 * another device, as an Apple device tapping "connect" does. */
static double connect_row(cairo_t *cr, double y) {
	if (!dev || jb(dev, "ready") || !strcmp(js(dev, "link", ""), "connected")) return y;
	if (!jb(root, "bluetooth")) return note(cr, y, "Bluetooth is off, so they can't be connected or heard.");
	if (!jb(dev, "in_bluez")) {
		y = note(cr, y, "They aren't paired with this phone. Pair them on the Bluetooth page.");
		zmc_add_hit(0, y - 4, zmc.width, 40, A_BLUETOOTH, 0);
		zmc_set_rgb(cr, zmc.accent);
		zmc_text(cr, "open Bluetooth", FONT_SEMILIGHT, 21, MARGIN, y, 0);
		return y + 44;
	}
	const char *state = js(dev, "connect", "idle");
	if (!strcmp(state, "busy")) return note(cr, y, "Connecting…");
	y = zmc_button(cr, MARGIN, y, 150, "connect here", A_CONNECT, 0) + 10;
	if (!strcmp(state, "failed")) {
		y = note(cr, y, "That didn't work. They may be in their closed case, out of range, or holding on to another device.");
	}
	return y;
}

static double draw_status(cairo_t *cr) {
	double y = tab_header(cr, P_STATUS);
	if (!root) {
		return note(cr, y, "The AirPods service isn't running, so nothing is known about them.");
	}
	if (!jb(root, "bluetooth")) y = note(cr, y, "Bluetooth is off. These are the last readings.");
	if (!dev) {
		y = note(cr, y, "No AirPods yet. Pair them on Settings' Bluetooth page. Once they have connected to this "
			"phone, their batteries show here even when they aren't connected.");
		zmc_add_hit(0, y - 4, zmc.width, 40, A_BLUETOOTH, 0);
		zmc_set_rgb(cr, zmc.accent);
		zmc_text(cr, "open Bluetooth", FONT_SEMILIGHT, 21, MARGIN, y, 0);
		return draw_nearby(cr, y + 44);
	}
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_line(cr, js(dev, "name", "AirPods"), FONT_SEMILIGHT, 26, MARGIN, y, zmc.width - 2 * MARGIN);
	y += 34;
	const char *link = js(dev, "link", "none");
	bool live = !strcmp(link, "aap");
	char line[128];
	if (live) snprintf(line, sizeof(line), "Connected");
	else if (!strcmp(link, "connected")) snprintf(line, sizeof(line), "Connected, reading from the AirPods…");
	else if (!jb(dev, "has_keys")) snprintf(line, sizeof(line), "Not connected. Battery can be read from broadcasts after they have connected here once.");
	else snprintf(line, sizeof(line), "%s", where_in_use());
	y = note(cr, y, line);
	y = connect_row(cr, y);

	y = batteries(cr, y, live);

	char where[96] = "";
	const char *lp = pods_place_text(js(jo(dev, "ear"), "left", "")), *rp = pods_place_text(js(jo(dev, "ear"), "right", ""));
	if (lp && rp) snprintf(where, sizeof(where), "Left %s, right %s", lp, rp);
	else if (lp) snprintf(where, sizeof(where), "Left %s", lp);
	else if (rp) snprintf(where, sizeof(where), "Right %s", rp);
	if (where[0]) {
		if (strcmp(js(dev, "ear_source", ""), "aap")) {
			char age[32], text[140];
			age_text(age, sizeof(age), jl(dev, "ear_seen", 0));
			snprintf(text, sizeof(text), "%s, %s", where, age);
			y = note(cr, y, text);
		} else {
			y = note(cr, y, where);
		}
	}
	const char *lid = js(dev, "lid", "unknown");
	if (strcmp(lid, "unknown")) {
		char text[64], age[32];
		age_text(age, sizeof(age), jl(dev, "lid_seen", 0));
		snprintf(text, sizeof(text), "Lid %s, %s", lid, age);
		y = note(cr, y, text);
	}
	if (!live) y = note(cr, y, "Heard from their broadcasts while this page is open. Levels can lag events by a little.");

	/* other pairs, if there are any */
	json_object *list = jo(root, "devices");
	int n = list ? (int)json_object_array_length(list) : 0;
	if (n > 1) y = section(cr, y + 6, "OTHER AIRPODS");
	for (int i = 0; i < n; i++) {
		json_object *d = json_object_array_get_idx(list, i);
		if (d == dev) continue;
		y = zmc_entry(cr, y, js(d, "name", "AirPods"), js(d, "model_label", ""), A_PICK, i);
	}
	return draw_nearby(cr, y);
}

/* A switch whose value may not be known: several settings never report theirs. */
static double switch_row(cairo_t *cr, double y, const char *title, int value, int action) {
	if (value >= 0) return zmc_switch(cr, y, title, value != 0, action);
	cairo_set_source_rgb(cr, 1, 1, 1);
	double label = zmc_switch_label_y(zmc_text(cr, title, FONT_SEMILIGHT, 21, MARGIN, y, zmc.width - 2 * MARGIN - 64));
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, settings_open() ? "Not reported by the AirPods. Tap to turn on." : "Not known", FONT_REGULAR, 13, MARGIN, y + label, zmc.width - 2 * MARGIN - 64);
	zmc_toggle(cr, zmc.width - MARGIN - 54, y + 4, false);
	zmc_add_hit(0, y - 4, zmc.width, 52 + label - ZMC_SWITCH_LINE, action, 0);
	return y + 66 + label - ZMC_SWITCH_LINE;
}

static double rows(cairo_t *cr, double y, int page) {
	double w = zmc.width - 2 * MARGIN;
	for (int i = 0; i < PODS_N_ROWS; i++) {
		const struct pods_row *r = &PODS_ROWS[i];
		if (r->page != page || !in_list(dev, "visible", r->key)) continue;
		int value = setting(r->key);
		/* a value the pods never reported is not shown as if they had */
		char caption[64];
		snprintf(caption, sizeof(caption), "%s%s", r->title, value < 0 ? " (not reported)" : "");
		switch (r->kind) {
		case PODS_R_SWITCH:
			y = switch_row(cr, y, r->title, value, A_ROW + i);
			break;
		case PODS_R_SLIDER:
			zmc_slider(cr, MARGIN, y, w, value < 0 ? 0 : value, caption, A_ROW + i);
			y += 66;
			break;
		case PODS_R_PICKER: {
			int sel = 0;
			for (int k = 0; k < r->count; k++) {
				if (r->values[k] == value) sel = k;
			}
			y = zmc_picker(cr, y, caption, r->options, r->count, sel, open_picker == A_ROW + i, A_ROW + i);
			break;
		}
		}
	}
	return y;
}

/* The top of a page of settings: why they can't be changed, if they can't, and how old they are.
 * Settings go over AAP only: no broadcast carries them, and nothing can set one without a link. */
static double settings_banner(cairo_t *cr, double y) {
	if (settings_open()) return y;
	if (ready()) return note(cr, y, "Reading settings from the AirPods…");
	if (!strcmp(js(dev, "link", ""), "connected")) return note(cr, y, "Connected, reading from the AirPods…");
	long seen = jl(dev, "link_seen", 0);
	char text[160], age[32];
	if (seen) {
		age_text(age, sizeof(age), seen);
		snprintf(text, sizeof(text), "Not connected to this phone. As they were when last connected, %s. They can only be changed with a connection.", age);
	} else {
		snprintf(text, sizeof(text), "Not connected to this phone. They can only be read and changed with a connection.");
	}
	y = note(cr, y, text);
	return connect_row(cr, y);
}

/* Rows drawn while they can't be changed: nothing under a finger, and looking it */
static void read_only_end(cairo_t *cr, double y0, double y1, int hits) {
	if (settings_open()) return;
	zmc.n_hits = hits;
	if (zmc.style == ZMC_STYLE_METRO) {
		cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
		cairo_rectangle(cr, 0, y0 - 4, zmc.width, y1 - y0 + 4);
		cairo_fill(cr);
	}
}

static bool mode_available(int i) {
	if (!strcmp(PODS_MODES[i].name, "adaptive") && !in_list(dev, "caps", "adaptive")) {
		return !strcmp(js(dev, "mode", ""), "adaptive");
	}
	return true;
}

static double draw_listening(cairo_t *cr) {
	double y = tab_header(cr, P_LISTEN);
	if (!dev) return note(cr, y, "No AirPods yet.");
	y = settings_banner(cr, y);
	double top = y;
	int hits = zmc.n_hits;
	if (in_list(dev, "caps", "modes")) {
		const char *mode = js(dev, "mode", js(dev, "last_mode", ""));
		if (!mode[0]) mode = js(dev, "last_mode", "");
		for (int i = 0; i < 4; i++) {
			if (!mode_available(i)) continue;
			bool on = !strcmp(mode, PODS_MODES[i].name);
			if (on) zmc_set_rgb(cr, zmc.accent);
			else cairo_set_source_rgb(cr, 1, 1, 1);
			zmc_text(cr, PODS_MODES[i].label, FONT_SEMILIGHT, 26, MARGIN, y, 0);
			zmc_add_hit(0, y - 4, zmc.width, 44, A_MODE, i);
			y += 44;
		}
		y += 10;
	} else {
		y = note(cr, y, "These AirPods have no listening modes.");
	}
	y = rows(cr, y, P_LISTEN);
	if (in_list(dev, "visible", "mode_cycle") && (settings_open() || setting("mode_cycle") >= 0)) {
		y = section(cr, y + 4, "PRESS AND HOLD SWITCHES BETWEEN");
		int mask = setting("mode_cycle");
		if (mask < 0) y = note(cr, y, "The AirPods don't report this. Choosing here replaces their list. At least two.");
		for (int i = 0; i < 4; i++) {
			if (!mode_available(i)) continue;
			y = zmc_switch_arg(cr, y, PODS_MODES[i].label, mask > 0 && (mask & PODS_MODES[i].bit), A_CYCLE, i);
		}
	}
	read_only_end(cr, top, y, hits);
	return y;
}

static double draw_controls(cairo_t *cr) {
	double y = tab_header(cr, P_CONTROLS);
	if (!dev) return note(cr, y, "No AirPods yet.");
	y = settings_banner(cr, y);
	double top = y;
	int hits = zmc.n_hits;
	y = rows(cr, y, P_CONTROLS);
	read_only_end(cr, top, y, hits);
	return y;
}

static double draw_about(cairo_t *cr) {
	double y = tab_header(cr, P_ABOUT);
	if (!dev) return note(cr, y, "No AirPods yet.");
	if (ready()) {
		y = zmc_textbox(cr, y + 24, "Name", name_edited ? name_edit : js(dev, "name", ""), editing, A_NAME, 0) + 6;
		if (editing) y = zmc_button(cr, MARGIN, y, 120, "save", A_NAME_SAVE, 0) + 8;
		y = note(cr, y, "Saved on the AirPods. This phone's Bluetooth list changes at their next connection, "
			"and Apple devices keep their own name for them.");
	} else {
		y = zmc_field(cr, y, "Name", js(dev, "name", "AirPods"));
		y = note(cr, y, "The name is kept on the AirPods: connect them to change it.");
		y = connect_row(cr, y);
	}
	y = zmc_field(cr, y, "Model", js(dev, "model_label", "AirPods"));
	const char *number = js(dev, "model_number", "");
	if (number[0]) y = zmc_field(cr, y, "Model number", number);
	const char *fw = js(dev, "firmware", "");
	if (fw[0]) y = zmc_field(cr, y, "Version", fw);
	long colour = jl(dev, "color", -1);
	if (colour >= 0 && colour < PODS_N_COLOURS) y = zmc_field(cr, y, "Colour", PODS_COLOURS[colour]);
	y = zmc_field(cr, y, "Address", js(dev, "mac", ""));
	y = zmc_field(cr, y, "Broadcasts", jb(dev, "has_keys") ? "Can be read: this phone has their keys" : "Can't be read until they have connected here once");
	if (jl(dev, "link_seen", 0) && !ready()) {
		char age[32];
		age_text(age, sizeof(age), jl(dev, "link_seen", 0));
		y = zmc_field(cr, y, "Last connected here", age);
	}
	if (!jb(dev, "tested")) {
		y = note(cr, y, "ztOS has been written against AirPods Pro 2. On this model some rows may not work: "
			"what they do comes from other projects' notes.");
	}
	if (!jb(dev, "ready") && jb(dev, "has_keys")) {
		y = zmc_button(cr, MARGIN, y + 4, forget_asked ? 180 : 120, forget_asked ? "really forget" : "forget",
			forget_asked ? A_FORGET_YES : A_FORGET, 0) + 8;
		y = note(cr, y, "Removes what this phone keeps to read their broadcasts, and their last readings. "
			"It doesn't unpair them; that's on the Bluetooth page.");
	}
	return y;
}

double pods_render(cairo_t *cr, int page) {
	switch (page - first_page) {
	case P_STATUS: return draw_status(cr);
	case P_LISTEN: return draw_listening(cr);
	case P_CONTROLS: return draw_controls(cr);
	case P_ABOUT: return draw_about(cr);
	}
	return 0;
}

/* ---- touch ---- */

static void save_name(void) {
	if (name_edited && name_edit[0] && strcmp(name_edit, js(dev, "name", ""))) {
		send_fmt("rename %s %s", js(dev, "mac", ""), name_edit);
	}
	editing = name_edited = false;
	zmc_keyboard(false);
}

void pods_activate(int action, int arg) {
	if (action >= A_ROW && action < A_ROW + PODS_N_ROWS && dev) {
		const struct pods_row *r = &PODS_ROWS[action - A_ROW];
		if (r->kind == PODS_R_SWITCH) {
			set_setting(r->key, setting(r->key) == 1 ? 0 : 1);
		} else if (r->kind == PODS_R_PICKER) {
			if (arg < 0) {
				open_picker = open_picker == action ? 0 : action;
			} else {
				if (arg < r->count) set_setting(r->key, r->values[arg]);
				open_picker = 0;
			}
		}
		zmc_redraw();
		return;
	}
	switch (action) {
	case A_PICK: {
		json_object *list = jo(root, "devices");
		if (list && arg < (int)json_object_array_length(list)) {
			snprintf(wanted, sizeof(wanted), "%s", js(json_object_array_get_idx(list, arg), "mac", ""));
			pick_device();
		}
		break;
	}
	case A_MODE:
		if (dev && arg >= 0 && arg < 4) {
			send_fmt("set %s listening_mode %s", js(dev, "mac", ""), PODS_MODES[arg].name);
			json_object_object_add(dev, "mode", json_object_new_string(PODS_MODES[arg].name));
		}
		break;
	case A_CYCLE: {
		if (!dev || arg < 0 || arg >= 4) break;
		int mask = setting("mode_cycle");
		if (mask < 0) mask = 0;
		mask ^= PODS_MODES[arg].bit;
		/* with fewer than two the daemon would refuse it: shown, sent when there are two */
		set_local("mode_cycle", mask);
		if (mask & (mask - 1)) send_fmt("set %s mode_cycle %d", js(dev, "mac", ""), mask);
		else last_local = zmc_now_ms();
		break;
	}
	case A_NAME:
		if (!editing) {
			editing = true;
			if (!name_edited) snprintf(name_edit, sizeof(name_edit), "%s", js(dev, "name", ""));
			name_edited = true;
			zmc_keyboard(true);
		}
		break;
	case A_NAME_SAVE: save_name(); break;
	case A_CONNECT:
		if (dev) {
			send_fmt("connect %s", js(dev, "mac", ""));
			json_object_object_add(dev, "connect", json_object_new_string("busy"));
		}
		break;
	case A_FORGET: forget_asked = true; break;
	case A_FORGET_YES:
		send_fmt("forget %s", js(dev, "mac", ""));
		forget_asked = false;
		wanted[0] = 0;
		last_local = 0;
		break;
	case A_BLUETOOTH:
		open_bluetooth();
		return;
	}
	zmc_redraw();
}

void pods_slide(int action, int pct, bool done) {
	static uint32_t sent;
	if (action < A_ROW || action >= A_ROW + PODS_N_ROWS || !dev) return;
	const struct pods_row *r = &PODS_ROWS[action - A_ROW];
	set_local(r->key, pct);
	if (done || zmc_now_ms() - sent > 120) {
		sent = zmc_now_ms();
		set_setting(r->key, pct);
	}
}

void pods_text(const char *t) {
	size_t n = strlen(name_edit);
	if (!strcmp(t, "\b")) {
		while (n > 0 && ((unsigned char)name_edit[n - 1] & 0xc0) == 0x80) n--;
		if (n > 0) n--;
		name_edit[n] = 0;
	} else if (!strcmp(t, "\n")) {
		save_name();
	} else if (n + strlen(t) <= MAX_NAME) {
		/* whole characters only: the pods would cut one in half */
		strcat(name_edit, t);
	}
	zmc_redraw();
}

bool pods_back(void) {
	if (open_picker) {
		open_picker = 0;
		zmc_redraw();
		return true;
	}
	if (editing) {
		editing = name_edited = false;
		zmc_keyboard(false);
		zmc_redraw();
		return true;
	}
	return false;
}

static void tick(void) {
	static uint32_t hinted, redrawn;
	uint32_t now = zmc_now_ms();
	/* "someone is looking": short bursts of listening, for as long as it is said */
	if (zmc.active && (!hinted || now - hinted > 10000)) {
		hinted = now;
		send_line("scan-hint on\n");
	}
	bool changed = false;
	if (now - last_local >= 1500 && !editing) changed = load_state(false);
	if (changed || now - redrawn > 30000) {	/* the ages move on by themselves */
		redrawn = now;
		zmc_redraw();
	}
}

void pods_arrived(int page) {
	open_picker = 0;
	zmc_every(1000, tick);
}

void pods_pages_setup(int first, void (*bluetooth)(void)) {
	first_page = first;
	open_bluetooth = bluetooth;
}

void pods_show(const char *mac) {
	snprintf(wanted, sizeof(wanted), "%s", mac ? mac : "");
	load_state(true);
	pick_device();
}

void pods_render_as_of(long when) {
	fixture_now = when;
}

