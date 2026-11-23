// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Editing the quick actions in the action centre itself (ROADMAP 8.49): the
 * EDIT button between CLEAR ALL and ALL SETTINGS, or a long press on a tile
 * that has no long press of its own, turns the panel into an editor. The
 * eight places stay where they are; under them a tray holds the tiles not in
 * use. A tile is dragged to another place (the ones between move along), out
 * to the tray (its place is left empty), or from the tray onto a place (what
 * was there goes back to the tray); a tap on a tray tile puts it in the first
 * empty place, a tap on a tile's minus takes it out. DONE, or the panel
 * closing, ends it. It is saved at every change to `quick` in
 * ~/.config/ztmetro/config, the key Settings > quick actions writes, so the
 * two agree.
 *
 * The list logic here is plain C (tests/shade-edit.c checks it on the PC);
 * the panel's part is shade-edit.c, included at the end of ztmetro.c.
 */
#ifndef ZT_SHADE_EDIT_H
#define ZT_SHADE_EDIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define SE_SLOTS 8

/* The place holding `q`, or -1. */
static inline int se_find(const int s[SE_SLOTS], int q) {
	for (int i = 0; i < SE_SLOTS; i++) {
		if (s[i] == q) return i;
	}
	return -1;
}

/* The first empty place (`none`), or -1. */
static inline int se_first_empty(const int s[SE_SLOTS], int none) {
	return se_find(s, none);
}

/* A tile dragged from place `from` to place `to`: it goes there, and the
 * ones between move along by one (as Start's tiles do). */
static inline void se_move(int s[SE_SLOTS], int from, int to) {
	if (from < 0 || from >= SE_SLOTS || to < 0 || to >= SE_SLOTS || from == to) return;
	int q = s[from];
	if (from < to) {
		memmove(&s[from], &s[from + 1], (size_t)(to - from) * sizeof(int));
	} else {
		memmove(&s[to + 1], &s[to], (size_t)(from - to) * sizeof(int));
	}
	s[to] = q;
}

/* Place `slot` taken out: left empty. */
static inline void se_remove(int s[SE_SLOTS], int slot, int none) {
	if (slot >= 0 && slot < SE_SLOTS) s[slot] = none;
}

/* Tile `q` from the tray onto place `slot`: what was there goes back to the
 * tray. A tile already in another place moves from there instead (no tile
 * twice). */
static inline void se_place(int s[SE_SLOTS], int slot, int q, int none) {
	if (slot < 0 || slot >= SE_SLOTS || q == none) return;
	int at = se_find(s, q);
	if (at >= 0 && at != slot) s[at] = none;
	s[slot] = q;
}

/* Tap on a tray tile: into the first empty place. False if there is none. */
static inline bool se_add(int s[SE_SLOTS], int q, int none) {
	if (q == none || se_find(s, q) >= 0) return false;
	int at = se_first_empty(s, none);
	if (at < 0) return false;
	s[at] = q;
	return true;
}

/* The tray: every tile 0 .. n-1 that is not in a place and is not `none`,
 * in that order. Returns how many. */
static inline int se_tray(const int s[SE_SLOTS], int n, int none, int *out) {
	int k = 0;
	for (int q = 0; q < n; q++) {
		if (q != none && se_find(s, q) < 0) out[k++] = q;
	}
	return k;
}

/* "wifi,data,...": the eight places by id, as `quick` keeps them. */
static inline void se_format(const int s[SE_SLOTS], const char *const ids[], char *out, size_t size) {
	size_t at = 0;
	out[0] = 0;
	for (int i = 0; i < SE_SLOTS && at < size; i++) {
		int n = snprintf(out + at, size - at, "%s%s", i ? "," : "", ids[s[i]]);
		if (n < 0) break;
		at += (size_t)n;
	}
}

/* `quick`'s value into the places, as the shell and Settings read it: up to
 * eight known ids in order, unknown words skipped; places past the list keep
 * what they had (the defaults). Returns how many were read. */
static inline int se_parse(const char *value, const char *const ids[], int n, int s[SE_SLOTS]) {
	char copy[256], *save = NULL;
	snprintf(copy, sizeof(copy), "%s", value ? value : "");
	int k = 0;
	for (char *word = strtok_r(copy, ", ", &save); word && k < SE_SLOTS; word = strtok_r(NULL, ", ", &save)) {
		for (int q = 0; q < n; q++) {
			if (strcmp(word, ids[q]) == 0) {
				s[k++] = q;
				break;
			}
		}
	}
	return k;
}

/* The tray's tile `k`, laid out as the places are: `across` to a row, each
 * w x h with `gap` between, from (left, top). */
static inline void se_tray_rect(int k, int across, double left, double top, double w, double h, double gap,
		double *x, double *y) {
	*x = left + (k % across) * (w + gap);
	*y = top + (k / across) * (h + gap);
}

/* The place a dragged tile's centre (cx, cy) is over: the nearest place whose
 * rectangle, grown by `slack`, holds it; -1 if none. rects[i] = x, y, w, h. */
static inline int se_slot_at(const double rects[SE_SLOTS][4], double cx, double cy, double slack) {
	int best = -1;
	double best_d = 0;
	for (int i = 0; i < SE_SLOTS; i++) {
		const double *r = rects[i];
		if (cx < r[0] - slack || cx >= r[0] + r[2] + slack || cy < r[1] - slack || cy >= r[1] + r[3] + slack) continue;
		double dx = cx - (r[0] + r[2] / 2), dy = cy - (r[1] + r[3] / 2), d = dx * dx + dy * dy;
		if (best < 0 || d < best_d) {
			best = i;
			best_d = d;
		}
	}
	return best;
}

#ifndef ZT_SHADE_EDIT_LOGIC_ONLY
struct zt_server;
struct touch_point;
/* the panel's editor (shade-edit.c) */
static bool shade_edit_active(void);
static void shade_edit_begin(struct zt_server *server);
static void shade_edit_end(struct zt_server *server);
/* shade_hit()'s links row: 8 CLEAR ALL, 11 EDIT, 9 ALL SETTINGS */
static int shade_edit_links_hit(struct zt_server *server, double x);
/* draw_shade(): the EDIT button in the links row */
static void shade_edit_draw_button(struct zt_server *server, cairo_t *cr, double links);
/* the long press on a tile with none of its own: the editor, with that tile in hand */
static bool shade_edit_long_press(struct zt_server *server, struct touch_point *p);
/* touches while it is open: all of them are the editor's */
static void shade_edit_down(struct zt_server *server, struct touch_point *p, double x, double y);
static void shade_edit_move(struct zt_server *server, struct touch_point *p, double x, double y);
static void shade_edit_up(struct zt_server *server, struct touch_point *p, bool cancelled);
/* the frame, over the panel's tiles */
static void shade_edit_draw(struct zt_server *server);
#endif

#endif
