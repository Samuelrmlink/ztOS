// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Editing the quick actions in the action centre itself (ROADMAP 8.49): the
 * EDIT button between CLEAR ALL and ALL SETTINGS turns the panel into an
 * editor (only the button: a held tile opens its own modal, as it did before
 * 8.49; 8.55). The places stay where they are, with an empty row under the
 * last for a tile to start a new one (two to four rows); under them a tray
 * holds the tiles not in use. A tile is dragged onto another tile (the ones
 * between move along) or onto an empty place (it simply goes there), out to
 * the tray (its place is left empty), or from the tray onto a place (what
 * was there goes back to the tray); a tap on a tray tile puts it in the
 * first empty place (a new row if there is none), a tap on a tile's minus
 * takes it out. DONE, or the panel
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

/* The places come in rows of four (a turned panel shows two of those to
 * its row of eight): two rows at the least, as before 8.55, and up to four.
 * `quick` keeps one id for each place, so a list of eight is the old two
 * rows and twelve or sixteen are three or four; nothing to migrate. */
#define SE_ROW		4
#define SE_MIN		8
#define SE_MAX		16
#define SE_SLOTS	SE_MAX	/* (8.49's name: the arrays' size) */

/* `n` rounded up to whole rows, within SE_MIN .. SE_MAX. */
static inline int se_rows_of(int n) {
	n = (n + SE_ROW - 1) / SE_ROW * SE_ROW;
	return n < SE_MIN ? SE_MIN : n > SE_MAX ? SE_MAX : n;
}

/* The place of the first `n` holding `q`, or -1. */
static inline int se_find(const int s[], int n, int q) {
	for (int i = 0; i < n; i++) {
		if (s[i] == q) return i;
	}
	return -1;
}

/* The first empty place (`none`), or -1. */
static inline int se_first_empty(const int s[], int n, int none) {
	return se_find(s, n, none);
}

/* The rows at the end with nothing in them, taken away (never below two).
 * Returns the places there are now. */
static inline int se_trim(int s[], int n, int none) {
	n = se_rows_of(n);
	while (n > SE_MIN) {
		bool empty = true;
		for (int i = n - SE_ROW; i < n; i++) empty = empty && s[i] == none;
		if (!empty) break;
		n -= SE_ROW;
	}
	return n;
}

/* The places grown to hold `slot` (a tile put in the empty row the editor
 * shows under the last): the new row's places empty. Returns the count. */
static inline int se_grow(int s[], int n, int slot, int none) {
	if (slot < n || slot >= SE_MAX) return n;
	int to = se_rows_of(slot + 1);
	for (int i = n; i < to; i++) s[i] = none;
	return to;
}

/* A tile dragged from place `from` to place `to`: it goes there, and the
 * ones between move along by one (as Start's tiles do). */
static inline void se_move(int s[], int n, int from, int to) {
	if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
	int q = s[from];
	if (from < to) {
		memmove(&s[from], &s[from + 1], (size_t)(to - from) * sizeof(int));
	} else {
		memmove(&s[to + 1], &s[to], (size_t)(from - to) * sizeof(int));
	}
	s[to] = q;
}

/* A tile let go over place `to`: onto an empty place it simply goes there
 * (its own is left empty, so that a tile taken to a new row doesn't pull
 * the rows above along); onto a tile, se_move. */
static inline void se_drop(int s[], int n, int from, int to, int none) {
	if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
	if (s[to] == none) {
		s[to] = s[from];
		s[from] = none;
	} else {
		se_move(s, n, from, to);
	}
}

/* Place `slot` taken out: left empty. */
static inline void se_remove(int s[], int n, int slot, int none) {
	if (slot >= 0 && slot < n) s[slot] = none;
}

/* Tile `q` from the tray onto place `slot`: what was there goes back to the
 * tray. A tile already in another place moves from there instead (no tile
 * twice). */
static inline void se_place(int s[], int n, int slot, int q, int none) {
	if (slot < 0 || slot >= n || q == none) return;
	int at = se_find(s, n, q);
	if (at >= 0 && at != slot) s[at] = none;
	s[slot] = q;
}

/* Tap on a tray tile: into the first empty place. False if there is none. */
static inline bool se_add(int s[], int n, int q, int none) {
	if (q == none || se_find(s, n, q) >= 0) return false;
	int at = se_first_empty(s, n, none);
	if (at < 0) return false;
	s[at] = q;
	return true;
}

/* The tray: every tile 0 .. n_ids-1 that is not in one of the `n` places and
 * is not `none`, in that order. Returns how many. */
static inline int se_tray(const int s[], int n, int n_ids, int none, int *out) {
	int k = 0;
	for (int q = 0; q < n_ids; q++) {
		if (q != none && se_find(s, n, q) < 0) out[k++] = q;
	}
	return k;
}

/* "wifi,data,...": the `n` places by id, as `quick` keeps them. */
static inline void se_format(const int s[], int n, const char *const ids[], char *out, size_t size) {
	size_t at = 0;
	out[0] = 0;
	for (int i = 0; i < n && at < size; i++) {
		int k = snprintf(out + at, size - at, "%s%s", i ? "," : "", ids[s[i]]);
		if (k < 0) break;
		at += (size_t)k;
	}
}

/* `quick`'s value into the places, as the shell and Settings read it: up to
 * sixteen known ids in order, unknown words skipped. Places past the list
 * keep what they had (the defaults) up to the second row's end; past that,
 * a part row is filled up with `none`. *n gets the places there are: the
 * rows the list reaches, two at the least (an older list of eight stays two
 * rows). Returns how many ids were read. */
static inline int se_parse(const char *value, const char *const ids[], int n_ids, int none,
		int s[SE_MAX], int *n) {
	char copy[400], *save = NULL;
	snprintf(copy, sizeof(copy), "%s", value ? value : "");
	int k = 0;
	for (char *word = strtok_r(copy, ", ", &save); word && k < SE_MAX; word = strtok_r(NULL, ", ", &save)) {
		for (int q = 0; q < n_ids; q++) {
			if (strcmp(word, ids[q]) == 0) {
				s[k++] = q;
				break;
			}
		}
	}
	for (int i = k > SE_MIN ? k : SE_MIN; i < SE_MAX; i++) s[i] = none;
	if (n) *n = se_rows_of(k);
	return k;
}

/* The tray's tile `k`, laid out as the places are: `across` to a row, each
 * w x h with `gap` between, from (left, top). */
static inline void se_tray_rect(int k, int across, double left, double top, double w, double h, double gap,
		double *x, double *y) {
	*x = left + (k % across) * (w + gap);
	*y = top + (k / across) * (h + gap);
}

/* The place a dragged tile's centre (cx, cy) is over: the nearest of the `n`
 * places whose rectangle, grown by `slack`, holds it; -1 if none.
 * rects[i] = x, y, w, h. */
static inline int se_slot_at(const double rects[][4], int n, double cx, double cy, double slack) {
	int best = -1;
	double best_d = 0;
	for (int i = 0; i < n; i++) {
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
/* shade_hit()'s links row: SHADE_CLEAR, SHADE_EDIT or SHADE_ALLSET */
static int shade_edit_links_hit(struct zt_server *server, double x);
/* draw_shade(): the EDIT button in the links row */
static void shade_edit_draw_button(struct zt_server *server, cairo_t *cr, double links);
/* touches while it is open: all of them are the editor's */
static void shade_edit_down(struct zt_server *server, struct touch_point *p, double x, double y);
static void shade_edit_move(struct zt_server *server, struct touch_point *p, double x, double y);
static void shade_edit_up(struct zt_server *server, struct touch_point *p, bool cancelled);
/* the frame, over the panel's tiles; x0: a turned side sheet's left edge */
static void shade_edit_draw(struct zt_server *server, float x0);
#endif

#endif
