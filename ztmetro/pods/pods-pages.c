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
#include "zmc-gesture.h"

#define MARGIN ZMC_MARGIN
#define MAX_NAME 127

enum page { P_STATUS, P_LISTEN, P_CONTROLS, P_ABOUT };

enum {
	A_PICK = PODS_ACTIONS + 1, A_MODE, A_CYCLE, A_NAME, A_NAME_SAVE, A_FORGET, A_FORGET_YES, A_BLUETOOTH,
	A_ROW = PODS_ACTIONS + 100,	/* + which row of ROWS */
};

enum row_kind { R_SWITCH, R_SLIDER, R_PICKER };
struct row {
	const char *key, *title;
	enum row_kind kind;
	int page;
	const char *const *options;	/* R_PICKER */
	const int *values;
	int count;
};

static const char *const O_MIC[] = { "Automatic", "Always right", "Always left" };
static const int V_012[] = { 0, 1, 2 };
static const char *const O_HOLD[] = { "Noise control", "Siri" };
static const int V_HOLD[] = { 1, 5 };
static const char *const O_SPEED[] = { "Default", "Slower", "Slowest" };
static const char *const O_SWIPE[] = { "Default", "Longer", "Longest" };

static const struct row ROWS[] = {
	{ "conversation", "Conversation Awareness", R_SWITCH, P_LISTEN },
	{ "adaptive_strength", "Adaptive strength", R_SLIDER, P_LISTEN },
	{ "personalized_volume", "Personalized Volume", R_SWITCH, P_LISTEN },
	{ "one_bud_anc", "ANC with one AirPod", R_SWITCH, P_LISTEN },
	{ "hold_left", "Press and hold, left", R_PICKER, P_CONTROLS, O_HOLD, V_HOLD, 2 },
	{ "hold_right", "Press and hold, right", R_PICKER, P_CONTROLS, O_HOLD, V_HOLD, 2 },
	{ "press_speed", "Press speed", R_PICKER, P_CONTROLS, O_SPEED, V_012, 3 },
	{ "hold_duration", "Press and hold duration", R_PICKER, P_CONTROLS, O_SPEED, V_012, 3 },
	{ "swipe_volume", "Swipe for volume", R_SWITCH, P_CONTROLS },
	{ "swipe_length", "Swipe length", R_PICKER, P_CONTROLS, O_SWIPE, V_012, 3 },
	{ "ear_detection", "Automatic ear detection", R_SWITCH, P_CONTROLS },
	{ "mic", "Microphone", R_PICKER, P_CONTROLS, O_MIC, V_012, 3 },
	{ "sleep_detection", "Pause when you fall asleep", R_SWITCH, P_CONTROLS },
	{ "chime_volume", "Chime volume", R_SLIDER, P_CONTROLS },
	{ "case_tone", "Case sounds", R_SWITCH, P_CONTROLS },
	{ "case_tone_volume", "Case sounds volume", R_SLIDER, P_CONTROLS },
	/* not shown: "connect_auto" (0x20). Only named in LibrePods' table, and it may
	 * change how the pods reconnect. pods-model.h keeps it for when it's been tried. */
};
#define N_ROWS ((int)(sizeof(ROWS) / sizeof(ROWS[0])))

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
	long s = now_s() - seen;
	if (s < 0) s = 0;
	if (s < 90) snprintf(out, size, "just now");
	else if (s < 5400) snprintf(out, size, "%ld minutes ago", (s + 30) / 60);
	else if (s < 129600) snprintf(out, size, "%ld hours ago", (s + 1800) / 3600);
	else snprintf(out, size, "%ld days ago", (s + 43200) / 86400);
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

/* One part: its name, its level large, a bar; and how old the reading is, if it is. */
static double battery_row(cairo_t *cr, double y, const char *label, json_object *b, bool live) {
	double w = zmc.width - 2 * MARGIN;
	json_object *lv = jo(b, "level");
	bool known = lv && json_object_is_type(lv, json_type_int);
	int level = known ? json_object_get_int(lv) : 0;
	bool charging = jb(b, "charging");
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, label, FONT_SEMILIGHT, 21, MARGIN, y, 0);
	char text[48];
	if (known) snprintf(text, sizeof(text), "%d%%%s", level, charging ? ", charging" : "");
	else snprintf(text, sizeof(text), "not read yet");
	if (known) {
		zmc_set_rgb(cr, zmc.accent);
		if (level <= 15 && !charging) cairo_set_source_rgb(cr, 0.9, 0.16, 0.16);
	} else {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	}
	double tw = zmc_text_width(text, FONT_REGULAR, 16);
	zmc_text(cr, text, FONT_REGULAR, 16, MARGIN + w - tw, y + 5, 0);
	double bar_y = y + 32;
	cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
	cairo_rectangle(cr, MARGIN, bar_y, w, 5);
	cairo_fill(cr);
	if (known) {
		if (level <= 15 && !charging) cairo_set_source_rgb(cr, 0.9, 0.16, 0.16);
		else zmc_set_rgb(cr, zmc.accent);
		cairo_rectangle(cr, MARGIN, bar_y, w * level / 100.0, 5);
		cairo_fill(cr);
	}
	y = bar_y + 11;
	if (known && !live) {
		char age[32], line[64];
		age_text(age, sizeof(age), jl(b, "seen", 0));
		snprintf(line, sizeof(line), "Last heard %s", age);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, line, FONT_REGULAR, 13, MARGIN, y, 0);
		y += 20;
	}
	return y + 10;
}

static const char *place_text(const char *place) {
	if (!strcmp(place, "in_ear")) return "in your ear";
	if (!strcmp(place, "out")) return "out";
	if (!strcmp(place, "case")) return "in the open case";
	if (!strcmp(place, "case_closed")) return "in the closed case";
	return NULL;
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
	const char *line = live ? "Connected" : !strcmp(link, "connected") ? "Connected, reading from the AirPods…" :
		jb(dev, "has_keys") ? "Not connected: read from their broadcasts" :
		"Not connected. Battery can be read from broadcasts after they have connected here once.";
	y = note(cr, y, line);

	json_object *bat = jo(dev, "battery");
	if (jo(bat, "headset")) {
		y = battery_row(cr, y, "Headphones", jo(bat, "headset"), live);
	} else {
		y = battery_row(cr, y, "Left", jo(bat, "left"), live);
		y = battery_row(cr, y, "Right", jo(bat, "right"), live);
		/* over a link too, the case says nothing when it is away: its reading has an age */
		json_object *kase = jo(bat, "case");
		y = battery_row(cr, y, "Case", kase, live && !strcmp(js(kase, "source", ""), "aap") &&
			now_s() - jl(kase, "seen", 0) < 300);
	}

	char where[96] = "";
	const char *lp = place_text(js(jo(dev, "ear"), "left", "")), *rp = place_text(js(jo(dev, "ear"), "right", ""));
	if (lp && rp) snprintf(where, sizeof(where), "Left %s, right %s", lp, rp);
	if (where[0]) y = note(cr, y, where);
	if (!live) {
		const char *lid = js(dev, "lid", "unknown");
		if (strcmp(lid, "unknown")) {
			char text[64], age[32];
			age_text(age, sizeof(age), jl(dev, "lid_seen", 0));
			snprintf(text, sizeof(text), "Lid %s, %s", lid, age);
			y = note(cr, y, text);
		}
		y = note(cr, y, "A closed case broadcasts nothing, and levels can lag by a few minutes. Open the "
			"lid to read the case.");
	}

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
	zmc_text(cr, "Not reported by the AirPods. Tap to turn on.", FONT_REGULAR, 13, MARGIN, y + label, zmc.width - 2 * MARGIN - 64);
	zmc_toggle(cr, zmc.width - MARGIN - 54, y + 4, false);
	zmc_add_hit(0, y - 4, zmc.width, 52 + label - ZMC_SWITCH_LINE, action, 0);
	return y + 66 + label - ZMC_SWITCH_LINE;
}

static double rows(cairo_t *cr, double y, int page) {
	double w = zmc.width - 2 * MARGIN;
	for (int i = 0; i < N_ROWS; i++) {
		const struct row *r = &ROWS[i];
		if (r->page != page || !in_list(dev, "visible", r->key)) continue;
		int value = setting(r->key);
		/* a value the pods never reported is not shown as if they had */
		char caption[64];
		snprintf(caption, sizeof(caption), "%s%s", r->title, value < 0 ? " (not reported)" : "");
		switch (r->kind) {
		case R_SWITCH:
			y = switch_row(cr, y, r->title, value, A_ROW + i);
			break;
		case R_SLIDER:
			zmc_slider(cr, MARGIN, y, w, value < 0 ? 0 : value, caption, A_ROW + i);
			y += 66;
			break;
		case R_PICKER: {
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

/* Why the rows below can't be used yet; NULL when they can. */
static const char *blocked(void) {
	if (!dev) return "No AirPods yet.";
	if (!ready()) return "Connect the AirPods to this phone to change these.";
	if (!settings_open()) return "Reading settings from the AirPods…";
	return NULL;
}

static const struct { const char *name, *label; int bit; } MODES[] = {
	{ "off", "Off", 1 }, { "anc", "Noise Cancellation", 2 },
	{ "transparency", "Transparency", 4 }, { "adaptive", "Adaptive", 8 },
};

static bool mode_available(int i) {
	if (!strcmp(MODES[i].name, "adaptive") && !in_list(dev, "caps", "adaptive")) {
		return !strcmp(js(dev, "mode", ""), "adaptive");
	}
	return true;
}

static double draw_listening(cairo_t *cr) {
	double y = tab_header(cr, P_LISTEN);
	const char *why = blocked();
	if (why) return note(cr, y, why);
	if (in_list(dev, "caps", "modes")) {
		const char *mode = js(dev, "mode", "");
		for (int i = 0; i < 4; i++) {
			if (!mode_available(i)) continue;
			bool on = !strcmp(mode, MODES[i].name);
			if (on) zmc_set_rgb(cr, zmc.accent);
			else cairo_set_source_rgb(cr, 1, 1, 1);
			zmc_text(cr, MODES[i].label, FONT_SEMILIGHT, 26, MARGIN, y, 0);
			zmc_add_hit(0, y - 4, zmc.width, 44, A_MODE, i);
			y += 44;
		}
		y += 10;
	} else {
		y = note(cr, y, "These AirPods have no listening modes.");
	}
	y = rows(cr, y, P_LISTEN);
	if (in_list(dev, "visible", "mode_cycle")) {
		y = section(cr, y + 4, "PRESS AND HOLD SWITCHES BETWEEN");
		int mask = setting("mode_cycle");
		if (mask < 0) y = note(cr, y, "The AirPods don't report this. Choosing here replaces their list. At least two.");
		for (int i = 0; i < 4; i++) {
			if (!mode_available(i)) continue;
			y = zmc_switch_arg(cr, y, MODES[i].label, mask > 0 && (mask & MODES[i].bit), A_CYCLE, i);
		}
	}
	return y;
}

static double draw_controls(cairo_t *cr) {
	double y = tab_header(cr, P_CONTROLS);
	const char *why = blocked();
	if (why) return note(cr, y, why);
	return rows(cr, y, P_CONTROLS);
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
		y = note(cr, y, "Connect them to change the name.");
	}
	y = zmc_field(cr, y, "Model", js(dev, "model_label", "AirPods"));
	const char *number = js(dev, "model_number", "");
	if (number[0]) y = zmc_field(cr, y, "Model number", number);
	const char *fw = js(dev, "firmware", "");
	if (fw[0]) y = zmc_field(cr, y, "Version", fw);
	y = zmc_field(cr, y, "Address", js(dev, "mac", ""));
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
	if (action >= A_ROW && action < A_ROW + N_ROWS && dev) {
		const struct row *r = &ROWS[action - A_ROW];
		if (r->kind == R_SWITCH) {
			set_setting(r->key, setting(r->key) == 1 ? 0 : 1);
		} else if (r->kind == R_PICKER) {
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
			send_fmt("set %s listening_mode %s", js(dev, "mac", ""), MODES[arg].name);
			json_object_object_add(dev, "mode", json_object_new_string(MODES[arg].name));
		}
		break;
	case A_CYCLE: {
		if (!dev || arg < 0 || arg >= 4) break;
		int mask = setting("mode_cycle");
		if (mask < 0) mask = 0;
		mask ^= MODES[arg].bit;
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
	if (action < A_ROW || action >= A_ROW + N_ROWS || !dev) return;
	const struct row *r = &ROWS[action - A_ROW];
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
	if (!hinted || now - hinted > 10000) {
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

