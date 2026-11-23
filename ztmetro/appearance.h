/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * How Start's tiles, its background photo, the app switcher and Hyperscan
 * look (ROADMAP 8.70): opacity, frost and brightness, a percent each, kept in
 * ~/.config/ztmetro/config. Settings and the shell share this table; a key
 * missing from the file is its default, and every default is the look from
 * before these existed:
 *
 *   background_photo 0: how much of Start's photo shows behind and between
 *     the tiles, from the theme's background (0, Windows Phone's way: the
 *     photo only in the tiles) to the whole photo (100); background_brightness
 *     dims it, background_blur blurs it;
 *   tile_fill 0: the tile's own accent fill over its window onto the photo,
 *     from see-through (0, the photo through the tile, as before) to solid
 *     (100); with no photo a tile is its solid colour whatever this says;
 *     tile_frost 0 and tile_brightness 100: the photo in them sharp, as before;
 *   switcher_opacity 100, search_opacity 100: the theme's background over
 *     what is behind them, as before; frost and brightness apply once the
 *     opacity is lowered (what shows through, blurred and dimmed);
 *   switcher_photo, search_photo off: with one on, what shows through is
 *     Start's photo instead, blurred by that frost (made once on the CPU,
 *     like the tiles' frost: no blur on the GPU as it opens).
 *
 * Retired with the first cut of 8.70 (config_key_retired drops their lines):
 * background_show (on became background_photo 100 at the brightness it had,
 * look_finish) and tile_opacity (the whole tile's alpha, never set).
 *
 * Settings tells the shell a slider's value live with `look KEY VALUE` on the
 * control pipe, and writes the file when it is let go without a SIGHUP (the
 * shell already has it). No wlroots or cairo here: tests/appearance.c.
 */
#ifndef ZT_APPEARANCE_H
#define ZT_APPEARANCE_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum look_key {
	LOOK_TILE_FILL, LOOK_TILE_FROST, LOOK_TILE_BRIGHTNESS,
	LOOK_BG_PHOTO, LOOK_BG_BRIGHTNESS, LOOK_BG_BLUR,
	LOOK_SWITCHER_OPACITY, LOOK_SWITCHER_FROST, LOOK_SWITCHER_BRIGHTNESS,
	LOOK_SEARCH_OPACITY, LOOK_SEARCH_FROST, LOOK_SEARCH_BRIGHTNESS,
	LOOK_SWITCHER_PHOTO, LOOK_SEARCH_PHOTO,
	N_LOOK
};

static const struct look_def {
	const char *key;
	int def, min, max;
	bool toggle;		/* on/off in the file, 1/0 here */
} LOOK_DEFS[N_LOOK] = {
	[LOOK_TILE_FILL] = { "tile_fill", 0, 0, 100, false },
	[LOOK_TILE_FROST] = { "tile_frost", 0, 0, 100, false },
	[LOOK_TILE_BRIGHTNESS] = { "tile_brightness", 100, 20, 100, false },
	[LOOK_BG_PHOTO] = { "background_photo", 0, 0, 100, false },
	[LOOK_BG_BRIGHTNESS] = { "background_brightness", 100, 0, 100, false },
	[LOOK_BG_BLUR] = { "background_blur", 0, 0, 100, false },
	[LOOK_SWITCHER_OPACITY] = { "switcher_opacity", 100, 0, 100, false },
	[LOOK_SWITCHER_FROST] = { "switcher_frost", 100, 0, 100, false },
	[LOOK_SWITCHER_BRIGHTNESS] = { "switcher_brightness", 100, 0, 100, false },
	[LOOK_SEARCH_OPACITY] = { "search_opacity", 100, 20, 100, false },
	[LOOK_SEARCH_FROST] = { "search_frost", 100, 0, 100, false },
	[LOOK_SEARCH_BRIGHTNESS] = { "search_brightness", 100, 0, 100, false },
	[LOOK_SWITCHER_PHOTO] = { "switcher_photo", 0, 0, 1, true },
	[LOOK_SEARCH_PHOTO] = { "search_photo", 0, 0, 1, true },
};

struct zt_look {
	int v[N_LOOK];
	bool seen[N_LOOK];	/* the file had it */
	int legacy_show;	/* background_show: -1 absent, 0 off, 1 on (look_finish) */
};

static inline void look_defaults(struct zt_look *l) {
	for (int k = 0; k < N_LOOK; k++) {
		l->v[k] = LOOK_DEFS[k].def;
		l->seen[k] = false;
	}
	l->legacy_show = -1;
}

/* The key's place in LOOK_DEFS; -1 if it is not one of these. */
static inline int look_index(const char *key) {
	for (int k = 0; k < N_LOOK; k++) {
		if (strcmp(key, LOOK_DEFS[k].key) == 0) return k;
	}
	return -1;
}

static inline int look_clamp(int k, int v) {
	return v < LOOK_DEFS[k].min ? LOOK_DEFS[k].min : v > LOOK_DEFS[k].max ? LOOK_DEFS[k].max : v;
}

/* A value as the file or the pipe says it: a number (a "%" after it is
 * allowed), or on/off for a switch. Anything else leaves it as it was. */
static inline bool look_value(int k, const char *value, int *out) {
	if (LOOK_DEFS[k].toggle) {
		if (!strcmp(value, "on") || !strcmp(value, "1") || !strcmp(value, "true")) *out = 1;
		else if (!strcmp(value, "off") || !strcmp(value, "0") || !strcmp(value, "false")) *out = 0;
		else return false;
		return true;
	}
	char *end;
	long v = strtol(value, &end, 10);
	if (end == value || (*end && strcmp(end, "%") != 0)) return false;
	*out = look_clamp(k, v < -1000 ? -1000 : v > 1000 ? 1000 : (int)v);
	return true;
}

/* key = value from the file: true when the key is one of these (a bad value
 * is then ignored, the key still taken) */
static inline bool look_parse(struct zt_look *l, const char *key, const char *value) {
	if (strcmp(key, "background_show") == 0) {
		l->legacy_show = !strcmp(value, "on") || !strcmp(value, "1") || !strcmp(value, "true");
		return true;
	}
	if (strcmp(key, "tile_opacity") == 0) return true;	/* retired: the whole tile's alpha */
	int k = look_index(key);
	if (k < 0) return false;
	int v;
	if (look_value(k, value, &v)) {
		l->v[k] = v;
		l->seen[k] = true;
	}
	return true;
}

/* After the file: the first cut's background_show on becomes the photo shown
 * whole, at the brightness it was drawn at (60 then). True when that changed
 * something, which the file must then be told (Settings writes it). */
static inline bool look_finish(struct zt_look *l) {
	if (l->legacy_show != 1 || l->seen[LOOK_BG_PHOTO]) return false;
	l->v[LOOK_BG_PHOTO] = 100;
	if (!l->seen[LOOK_BG_BRIGHTNESS]) l->v[LOOK_BG_BRIGHTNESS] = 60;
	return true;
}

/* As the file has it. */
static inline void look_format(const struct zt_look *l, int k, char *out, size_t size) {
	if (LOOK_DEFS[k].toggle) snprintf(out, size, "%s", l->v[k] ? "on" : "off");
	else snprintf(out, size, "%d", l->v[k]);
}

/* "KEY VALUE" (the pipe's `look` command, after the word): the key's index if
 * it was taken and its value changed, -1 if not (unknown, bad, the same). */
static inline int look_line(struct zt_look *l, const char *line) {
	char key[48];
	int n = 0;
	while (line[n] && line[n] != ' ' && n < (int)sizeof(key) - 1) {
		key[n] = line[n];
		n++;
	}
	key[n] = 0;
	if (line[n] != ' ') return -1;
	int k = look_index(key), v;
	if (k < 0) return -1;
	const char *value = line + n + 1;
	char buf[16];
	snprintf(buf, sizeof(buf), "%s", value);
	buf[strcspn(buf, " \t\r\n")] = 0;
	if (!look_value(k, buf, &v) || v == l->v[k]) return -1;
	l->v[k] = v;
	return k;
}

/* The fractions the renderer uses */
static inline float look_f(const struct zt_look *l, int k) {
	return (float)l->v[k] / 100.0f;
}

/* The blur radii, in pixels of the small copy of Start's photo (about 180
 * pixels across, wall-blur.h): 0 none, else 2 to 24 */
static inline int look_blur_radius(int percent) {
	return percent <= 0 ? 0 : 2 + (percent * 22 + 50) / 100;
}

#endif
