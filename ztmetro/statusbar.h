/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The status bar's icons, as the owner arranges them (ROADMAP 8.77): which
 * of them show, in what order, and the colour of an icon in each of its
 * states. Settings > customization > status bar edits it; the shell lays the
 * row out from it. Kept in ~/.config/ztmetro/config:
 *
 *   statusbar_icons = signal,data,wifi,location,bluetooth
 *       the left-hand icons in order; a "-" before a name hides it. A name
 *       left out (an older file, a new icon) goes at the end, shown. The
 *       battery and the clock are fixed at the right: the row is laid out
 *       from them (the plug's layer, the turned screen's two-line clock).
 *   statusbar_bt_off = hidden          Bluetooth off
 *   statusbar_bt_on = default          on, nothing connected (default: dim)
 *   statusbar_bt_connected = default   a device connected (default: the ink)
 *   statusbar_bt_streaming = default   and sound going to it (default: as connected)
 *       a colour: default, ink, dim, accent, hidden, a Metro colour's name
 *       (lime, green, ... as the accent picker's) or #rrggbb
 *   statusbar_bt_stream = dot          while streaming: dot (a dot beside the
 *       icon), colour (only the icon's colour) or pulse (a slow breathe of its
 *       opacity; steady when the status bar's animations are off)
 *   statusbar_bt_dot = accent          the dot's colour
 *
 * A key missing from the file is its default, and every default is the bar
 * from before 8.77: the white/dim rune, an accent dot while sound streams.
 * Settings tells the shell a change live with `look KEY VALUE` on the
 * control pipe (as appearance.h's keys) and writes the file without a SIGHUP.
 *
 * The colours are per icon and state through SB_COLOURED, so Wi-Fi or
 * location can be given states later; only Bluetooth has them now.
 *
 * No wlroots or cairo here: tests/statusbar.c.
 */
#ifndef ZT_STATUSBAR_H
#define ZT_STATUSBAR_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>

/* The icons, the movable ones first. */
enum sb_icon {
	SB_SIGNAL,	/* the bars (the aeroplane in airplane mode) and their data arrow */
	SB_DATA,	/* 4G, H+, 3G... while the data goes over the mobile network */
	SB_WIFI,	/* the fan and its data arrow, while connected */
	SB_LOCATION,	/* while the GNSS engine runs for someone */
	SB_BT,		/* the rune, and the device's battery beside it */
	SB_BATTERY,	/* fixed, at the right */
	SB_CLOCK,	/* fixed, the rightmost */
	SB_N
};
#define SB_N_MOVABLE	5

static const struct sb_icon_def {
	const char *key;	/* its name in statusbar_icons */
	const char *name;	/* Settings' row */
	const char *about;	/* the line under it */
} SB_ICONS[SB_N] = {
	[SB_SIGNAL] = { "signal", "Mobile signal", "Bars, or the aeroplane" },
	[SB_DATA] = { "data", "Data type", "4G, H+, 3G\u2026 on mobile data" },
	[SB_WIFI] = { "wifi", "Wi-Fi", "While connected" },
	[SB_LOCATION] = { "location", "Location", "While an app uses GPS" },
	[SB_BT] = { "bluetooth", "Bluetooth", "While on; a device's battery beside it" },
	[SB_BATTERY] = { "battery", "Battery", "Always shown, at the right" },
	[SB_CLOCK] = { "clock", "Clock", "Always shown, at the right" },
};

/* ---- colours ------------------------------------------------------------ */

/* A colour: one of these, or 0xRRGGBB (>= 0). */
enum {
	SB_C_DEFAULT = -1,	/* what the state had before 8.77 */
	SB_C_HIDDEN = -2,	/* the icon is not drawn in this state */
	SB_C_INK = -3,		/* the theme's ink (white in dark, black in light) */
	SB_C_DIM = -4,		/* halfway to the background: on but not connected */
	SB_C_ACCENT = -5,	/* the accent, whatever it is now */
};

/* The palette Settings offers: the theme's own, then the accent picker's
 * Metro colours (settings' ACCENTS, the shell's parse_color: the same). */
static const struct sb_swatch { const char *name, *label; int colour; } SB_PALETTE[] = {
	{ "default", "Default", SB_C_DEFAULT },
	{ "ink", "White (theme)", SB_C_INK },
	{ "dim", "Dim", SB_C_DIM },
	{ "accent", "Accent", SB_C_ACCENT },
	{ "hidden", "Hidden", SB_C_HIDDEN },
	{ "lime", "Lime", 0xa4c400 }, { "green", "Green", 0x60a917 }, { "emerald", "Emerald", 0x008a00 },
	{ "teal", "Teal", 0x00aba9 }, { "cyan", "Cyan", 0x1ba1e2 }, { "cobalt", "Cobalt", 0x0050ef },
	{ "indigo", "Indigo", 0x6a00ff }, { "violet", "Violet", 0xaa00ff }, { "pink", "Pink", 0xf472d0 },
	{ "magenta", "Magenta", 0xd80073 }, { "crimson", "Crimson", 0xa20025 }, { "red", "Red", 0xe51400 },
	{ "orange", "Orange", 0xfa6800 }, { "amber", "Amber", 0xf0a30a }, { "yellow", "Yellow", 0xe3c800 },
	{ "brown", "Brown", 0x825a2c }, { "olive", "Olive", 0x6d8764 }, { "steel", "Steel", 0x647687 },
	{ "mauve", "Mauve", 0x76608a }, { "taupe", "Taupe", 0x87794e },
};
#define SB_N_PALETTE	(int)(sizeof(SB_PALETTE) / sizeof(SB_PALETTE[0]))
#define SB_PALETTE_THEMED	5	/* the first five are not fixed colours */

/* The swatch a colour is, or -1 (a #rrggbb of its own). */
static inline int sb_swatch_of(int colour) {
	for (int i = 0; i < SB_N_PALETTE; i++) {
		if (SB_PALETTE[i].colour == colour) return i;
	}
	return -1;
}

/* A colour as the file or the pipe says it; false if it is none. */
static inline bool sb_colour_parse(const char *text, int *out) {
	for (int i = 0; i < SB_N_PALETTE; i++) {
		if (strcasecmp(text, SB_PALETTE[i].name) == 0) {
			*out = SB_PALETTE[i].colour;
			return true;
		}
	}
	if (strcasecmp(text, "white") == 0) {	/* (the label's word) */
		*out = SB_C_INK;
		return true;
	}
	const char *hex = text[0] == '#' ? text + 1 : text;
	if (strlen(hex) != 6) return false;
	char *end;
	long v = strtol(hex, &end, 16);
	if (*end || v < 0) return false;
	*out = (int)v;
	return true;
}

static inline void sb_colour_format(int colour, char *out, size_t size) {
	int i = sb_swatch_of(colour);
	if (i >= 0) snprintf(out, size, "%s", SB_PALETTE[i].name);
	else snprintf(out, size, "#%06x", (unsigned)colour & 0xffffff);
}

/* Icons with a colour for each state. Each state's default is what it had
 * before; a state whose default is "as another" names it in `like`. */
#define SB_MAX_STATES	4
struct sb_state_def {
	const char *key;	/* statusbar_<icon>_<key> */
	const char *name;	/* Settings' row */
	int def;		/* SB_C_DEFAULT resolves to this (never SB_C_DEFAULT itself) */
	int like;		/* >= 0: default to that state's colour instead */
};

enum sb_bt_state { SB_BT_OFF, SB_BT_ON, SB_BT_CONNECTED, SB_BT_STREAMING, SB_BT_N_STATES };

static const struct sb_state_def SB_BT_STATES[SB_BT_N_STATES] = {
	[SB_BT_OFF] = { "off", "Off", SB_C_HIDDEN, -1 },
	[SB_BT_ON] = { "on", "On, nothing connected", SB_C_DIM, -1 },
	[SB_BT_CONNECTED] = { "connected", "Connected", SB_C_INK, -1 },
	[SB_BT_STREAMING] = { "streaming", "Streaming audio", SB_C_INK, SB_BT_CONNECTED },
};

static const struct sb_coloured {
	int icon;
	const char *prefix;	/* statusbar_<prefix>_<state> */
	int n;
	const struct sb_state_def *states;
} SB_COLOURED[] = {
	{ SB_BT, "bt", SB_BT_N_STATES, SB_BT_STATES },
	/* later: { SB_WIFI, "wifi", ... }, { SB_LOCATION, "location", ... } */
};
#define SB_N_COLOURED	(int)(sizeof(SB_COLOURED) / sizeof(SB_COLOURED[0]))
#define SB_COLOURED_BT	0	/* Bluetooth's place in SB_COLOURED */

/* What shows while sound streams to the Bluetooth device. */
enum { SB_STREAM_DOT, SB_STREAM_COLOUR, SB_STREAM_PULSE, SB_N_STREAM };
static const char *const SB_STREAM_KEYS[SB_N_STREAM] = { "dot", "colour", "pulse" };
static const char *const SB_STREAM_NAMES[SB_N_STREAM] = {
	"Dot beside the icon", "Icon colour only", "Slow pulse",
};

/* ---- the whole of it ---------------------------------------------------- */

struct zt_statusbar {
	int order[SB_N_MOVABLE];	/* the movable icons, left to right */
	bool shown[SB_N];		/* the fixed two always are */
	int colour[SB_N_COLOURED][SB_MAX_STATES];
	int bt_stream;			/* SB_STREAM_ */
	int bt_dot;			/* the dot's colour */
};

static inline void sb_defaults(struct zt_statusbar *s) {
	for (int i = 0; i < SB_N_MOVABLE; i++) s->order[i] = i;
	for (int i = 0; i < SB_N; i++) s->shown[i] = true;
	for (int c = 0; c < SB_N_COLOURED; c++) {
		for (int k = 0; k < SB_MAX_STATES; k++) s->colour[c][k] = SB_C_DEFAULT;
	}
	s->bt_stream = SB_STREAM_DOT;
	s->bt_dot = SB_C_ACCENT;
}

static inline int sb_icon_named(const char *name) {
	for (int i = 0; i < SB_N; i++) {
		if (strcmp(name, SB_ICONS[i].key) == 0) return i;
	}
	return -1;
}

/* "signal,-data,wifi": the order and which are hidden. Unknown and fixed
 * names, and a name said twice, are passed over; the ones not named follow
 * in their usual order, shown. Never fails: the worst is the default. */
static inline void sb_icons_parse(struct zt_statusbar *s, const char *text) {
	bool seen[SB_N] = { false };
	int n = 0;
	char buf[256];
	snprintf(buf, sizeof(buf), "%s", text);
	for (char *save = NULL, *tok = strtok_r(buf, ", \t", &save); tok; tok = strtok_r(NULL, ", \t", &save)) {
		bool hide = tok[0] == '-' || tok[0] == '!';
		int i = sb_icon_named(hide ? tok + 1 : tok);
		if (i < 0 || i >= SB_N_MOVABLE || seen[i]) continue;
		seen[i] = true;
		s->order[n++] = i;
		s->shown[i] = !hide;
	}
	for (int i = 0; i < SB_N_MOVABLE; i++) {
		if (!seen[i]) {
			s->order[n++] = i;
			s->shown[i] = true;
		}
	}
}

static inline void sb_icons_format(const struct zt_statusbar *s, char *out, size_t size) {
	size_t at = 0;
	out[0] = 0;
	for (int i = 0; i < SB_N_MOVABLE && at < size; i++) {
		int k = s->order[i];
		int w = snprintf(out + at, size - at, "%s%s%s", i ? "," : "", s->shown[k] ? "" : "-", SB_ICONS[k].key);
		if (w < 0) break;
		at += (size_t)w;
	}
}

/* An icon moved from place `from` to place `to` of the order; the ones
 * between move along by one. */
static inline void sb_move(struct zt_statusbar *s, int from, int to) {
	if (from < 0 || from >= SB_N_MOVABLE || to < 0 || to >= SB_N_MOVABLE || from == to) return;
	int k = s->order[from];
	if (from < to) memmove(&s->order[from], &s->order[from + 1], (size_t)(to - from) * sizeof(int));
	else memmove(&s->order[to + 1], &s->order[to], (size_t)(from - to) * sizeof(int));
	s->order[to] = k;
}

/* The place of an icon in the order, -1 for a fixed one. */
static inline int sb_place(const struct zt_statusbar *s, int icon) {
	for (int i = 0; i < SB_N_MOVABLE; i++) {
		if (s->order[i] == icon) return i;
	}
	return -1;
}

/* ---- keys ---------------------------------------------------------------- */

/* Every key, in the order Settings writes them: statusbar_icons, each
 * coloured icon's states, then Bluetooth's streaming style and dot. */
static inline int sb_n_keys(void) {
	int n = 1 + 2;
	for (int c = 0; c < SB_N_COLOURED; c++) n += SB_COLOURED[c].n;
	return n;
}

/* The k-th key's name (kept: the pointer stays good). */
static inline const char *sb_key_name(int k) {
	static char names[1 + 2 + SB_N_COLOURED * SB_MAX_STATES][48];
	if (k == 0) return "statusbar_icons";
	int at = 1;
	for (int c = 0; c < SB_N_COLOURED; c++) {
		for (int i = 0; i < SB_COLOURED[c].n; i++, at++) {
			if (at == k) {
				if (!names[k][0]) {
					snprintf(names[k], sizeof(names[k]), "statusbar_%s_%s", SB_COLOURED[c].prefix,
						SB_COLOURED[c].states[i].key);
				}
				return names[k];
			}
		}
	}
	if (k == at) return "statusbar_bt_stream";
	if (k == at + 1) return "statusbar_bt_dot";
	return NULL;
}

static inline int sb_key_index(const char *key) {
	if (strncmp(key, "statusbar_", 10) != 0) return -1;
	for (int k = 0; k < sb_n_keys(); k++) {
		if (strcmp(key, sb_key_name(k)) == 0) return k;
	}
	return -1;
}

/* The colour slot a key sets, or NULL if it is not a colour key. */
static inline int *sb_key_colour(struct zt_statusbar *s, int k) {
	int at = 1;
	for (int c = 0; c < SB_N_COLOURED; c++) {
		for (int i = 0; i < SB_COLOURED[c].n; i++, at++) {
			if (at == k) return &s->colour[c][i];
		}
	}
	return k == at + 1 ? &s->bt_dot : NULL;
}

static inline void sb_format(const struct zt_statusbar *s, int k, char *out, size_t size) {
	if (k == 0) {
		sb_icons_format(s, out, size);
		return;
	}
	int *c = sb_key_colour((struct zt_statusbar *)s, k);
	if (c) sb_colour_format(*c, out, size);
	else snprintf(out, size, "%s", SB_STREAM_KEYS[s->bt_stream]);
}

/* key = value: true when the key is one of these (a bad value is then
 * ignored, the key still taken). */
static inline bool sb_parse(struct zt_statusbar *s, const char *key, const char *value) {
	int k = sb_key_index(key);
	if (k < 0) return false;
	if (k == 0) {
		sb_icons_parse(s, value);
		return true;
	}
	int *c = sb_key_colour(s, k), v;
	if (c) {
		if (sb_colour_parse(value, &v)) *c = v;
		return true;
	}
	for (int i = 0; i < SB_N_STREAM; i++) {
		if (strcasecmp(value, SB_STREAM_KEYS[i]) == 0) s->bt_stream = i;
	}
	if (strcasecmp(value, "color") == 0) s->bt_stream = SB_STREAM_COLOUR;
	return true;
}

static inline bool sb_same(const struct zt_statusbar *a, const struct zt_statusbar *b) {
	return memcmp(a->order, b->order, sizeof(a->order)) == 0 && memcmp(a->shown, b->shown, sizeof(a->shown)) == 0 &&
		memcmp(a->colour, b->colour, sizeof(a->colour)) == 0 && a->bt_stream == b->bt_stream &&
		a->bt_dot == b->bt_dot;
}

/* "KEY VALUE" (the pipe's `look` command, after the word): true if the key
 * is one of these and something changed. */
static inline bool sb_line(struct zt_statusbar *s, const char *line) {
	char key[48], value[256];
	int n = 0;
	while (line[n] && line[n] != ' ' && n < (int)sizeof(key) - 1) {
		key[n] = line[n];
		n++;
	}
	key[n] = 0;
	if (line[n] != ' ' || sb_key_index(key) < 0) return false;
	snprintf(value, sizeof(value), "%s", line + n + 1);
	value[strcspn(value, "\r\n")] = 0;
	struct zt_statusbar before = *s;
	sb_parse(s, key, value);
	return !sb_same(&before, s);
}

/* ---- resolving ----------------------------------------------------------- */

/* One colour as 0xRRGGBB, `def` standing for SB_C_DEFAULT; false: hidden. */
static inline bool sb_colour_rgb(int c, uint32_t ink, uint32_t dim, uint32_t accent, uint32_t def, uint32_t *out) {
	switch (c) {
	case SB_C_HIDDEN: return false;
	case SB_C_DEFAULT: *out = def; return true;
	case SB_C_INK: *out = ink; return true;
	case SB_C_DIM: *out = dim; return true;
	case SB_C_ACCENT: *out = accent; return true;
	}
	*out = (uint32_t)c & 0xffffff;
	return true;
}

/* The colour an icon is drawn in, in `state`, as 0xRRGGBB from the theme's
 * ink, its dim and the accent; false: hidden in that state. */
static inline bool sb_resolve(const struct zt_statusbar *s, int coloured, int state, uint32_t ink, uint32_t dim,
		uint32_t accent, uint32_t *out) {
	const struct sb_coloured *ci = &SB_COLOURED[coloured];
	if (state < 0 || state >= ci->n) return false;
	int c = s->colour[coloured][state];
	for (int depth = 0; c == SB_C_DEFAULT && depth < SB_MAX_STATES; depth++) {
		const struct sb_state_def *d = &ci->states[state];
		if (d->like >= 0) {
			state = d->like;
			c = s->colour[coloured][state];
		} else {
			c = d->def;
		}
	}
	return sb_colour_rgb(c, ink, dim, accent, ink, out);
}

/* Bluetooth's state from what ztos-bt says. */
static inline int sb_bt_state(bool on, bool connected, bool streaming) {
	return !on ? SB_BT_OFF : !connected ? SB_BT_ON : streaming ? SB_BT_STREAMING : SB_BT_CONNECTED;
}

/* The pulse: a slow, smooth breathe of the icon's opacity, from full down to
 * SB_PULSE_LOW and back, once a period, as a cosine (no edges, no blink). */
#define SB_PULSE_MS	2800.0
#define SB_PULSE_LOW	0.30
#define SB_PULSE_TICK_MS	80	/* the bar redrawn about 12 times a second while it breathes */

static inline double sb_pulse_alpha(double t_ms, double period_ms) {
	if (period_ms <= 0) return 1.0;
	double phase = fmod(t_ms, period_ms) / period_ms;
	return SB_PULSE_LOW + (1.0 - SB_PULSE_LOW) * (0.5 + 0.5 * cos(2 * M_PI * phase));
}

#endif
