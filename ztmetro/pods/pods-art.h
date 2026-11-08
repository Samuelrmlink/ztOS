// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The status page's picture, shared by ztMETRO's page (pods-pages.c, cairo) and the
 * desktop app (qt/hero.cpp, QPainter): the buds and the case drawn from a few shapes,
 * a ring under each for its battery, and which parts share a ring. Here are the shapes,
 * the colours and where everything goes; each front end only turns them into strokes.
 * The drawings are ztOS's own: rounded shapes, not Apple's pictures.
 *
 * Both buds have one ring when they say the same level and the same charging; otherwise
 * one each. A bud with no reading has no ring while the other has one.
 */
#ifndef PODS_ART_H
#define PODS_ART_H
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum { PODS_PART_LEFT = 1, PODS_PART_RIGHT = 2, PODS_PART_CASE = 4, PODS_PART_HEADSET = 8 };

struct pods_battery { bool known; int level; bool charging; };

/* a ring: the parts it stands for, and their reading */
struct pods_ring { int parts; struct pods_battery b; };

static inline bool pods_battery_low(const struct pods_battery *b) { return b->known && b->level <= 15 && !b->charging; }

/* The rings for a pair: out has room for 3. Returns how many. */
static inline int pods_rings(const struct pods_battery *l, const struct pods_battery *r, const struct pods_battery *c,
		struct pods_ring *out) {
	int n = 0;
	if (l->known && r->known && (l->level != r->level || l->charging != r->charging)) {
		out[n].parts = PODS_PART_LEFT, out[n++].b = *l;
		out[n].parts = PODS_PART_RIGHT, out[n++].b = *r;
	} else if (l->known != r->known) {
		out[n].parts = l->known ? PODS_PART_LEFT : PODS_PART_RIGHT, out[n++].b = l->known ? *l : *r;
	} else {
		out[n].parts = PODS_PART_LEFT | PODS_PART_RIGHT, out[n++].b = *l;	/* the same, or neither known */
	}
	out[n].parts = PODS_PART_CASE, out[n++].b = *c;
	return n;
}

/* the text beside a ring's badge */
static inline void pods_ring_text(char *out, size_t size, const struct pods_ring *r) {
	if (r->b.known) snprintf(out, size, "%d%%", r->b.level);
	else snprintf(out, size, "–");
}

/* what a ring is, in words: for its "last heard" line, and for screen readers */
static inline const char *pods_ring_name(int parts) {
	switch (parts) {
	case PODS_PART_LEFT: return "Left";
	case PODS_PART_RIGHT: return "Right";
	case PODS_PART_LEFT | PODS_PART_RIGHT: return "Left and right";
	case PODS_PART_CASE: return "Case";
	default: return "Headphones";
	}
}

/* How old a ring's reading is: "Left: last heard 5 minutes ago, from the case". name NULL
 * for all of them at once. from: the reading's "source". */
static inline void pods_heard_text(char *out, size_t size, const char *name, const char *age, const char *from) {
	const char *how = !strcmp(from, "case") ? ", from the case" : !strcmp(from, "ble-coarse") ? ", to the nearest 10%" : "";
	if (name) snprintf(out, size, "%s: last heard %s%s", name, age, how);
	else snprintf(out, size, "Last heard %s%s", age, how);
}

/* ---- colours, 0xRRGGBB ---- */
#define PODS_C_FULL	0x34c759	/* a ring's level */
#define PODS_C_LOW	0xe5383b	/* 15% or less, not charging */
#define PODS_C_BODY_TOP	0xfafafc	/* the plastic, lit from above */
#define PODS_C_BODY_BOT	0xd6d6dc
#define PODS_C_EDGE	0x9a9aa2	/* its outline: the drawing is white on any background */
#define PODS_C_TIP	0xc4c4cb	/* ear tips */
#define PODS_C_DARK	0x2b2b30	/* the stem's sensor */
#define PODS_C_METAL	0xa6a6ae
#define PODS_C_SEAM	0xb4b4bc	/* the lid's line */
#define PODS_C_LED_OFF	0xb8b8be
#define PODS_C_LED_ON	0x3ddc68	/* the lid is open: the case's light is on */
#define PODS_C_BADGE	0x8e8e93	/* the L and R discs */

/* ---- the shapes ---- */
enum pods_shape_kind { PODS_S_ELLIPSE, PODS_S_RRECT, PODS_S_LINE };
enum pods_paint { PODS_P_BODY, PODS_P_TIP, PODS_P_DARK, PODS_P_METAL, PODS_P_SEAM, PODS_P_LED };

/* Ellipse and rounded rectangle: the box x, y, w, h, and r the corners' radius.
 * Line: from x, y by w, h, r wide. Body and tip shapes are outlined: every one of them
 * stroked first in PODS_C_EDGE, then all filled, so where they meet there is no line. */
struct pods_shape { unsigned char kind, paint, pro_only; float x, y, w, h, r; };

/* the left bud, from the front, in a 64 x 100 box; the right is its mirror */
#define PODS_BUD_W 64.0f
#define PODS_BUD_H 100.0f
static const struct pods_shape PODS_BUD[] = {
	{ PODS_S_ELLIPSE, PODS_P_TIP, 1, 2, 18, 30, 26, 0 },	/* ear tip, out to the side */
	{ PODS_S_ELLIPSE, PODS_P_BODY, 0, 12, 2, 46, 42, 0 },	/* the head */
	{ PODS_S_RRECT, PODS_P_BODY, 0, 26, 18, 18, 80, 9 },	/* the stem */
	{ PODS_S_ELLIPSE, PODS_P_DARK, 0, 30.5f, 12, 9, 22, 0 },	/* sensor */
	{ PODS_S_RRECT, PODS_P_METAL, 0, 27.5f, 88, 15, 8, 4 },	/* the stem's end */
};
#define PODS_N_BUD ((int)(sizeof(PODS_BUD) / sizeof(PODS_BUD[0])))

/* the case, lid closed, from the front, in a 150 x 108 box */
#define PODS_CASE_W 150.0f
#define PODS_CASE_H 108.0f
static const struct pods_shape PODS_CASE[] = {
	{ PODS_S_RRECT, PODS_P_BODY, 0, 2, 2, 146, 104, 34 },
	{ PODS_S_LINE, PODS_P_SEAM, 0, 4, 32, 142, 0, 1.2f },	/* lid */
	{ PODS_S_ELLIPSE, PODS_P_LED, 0, 72.5f, 52, 5, 5, 0 },
};
#define PODS_N_CASE ((int)(sizeof(PODS_CASE) / sizeof(PODS_CASE[0])))

/* a shape as drawn: in a box at x, y, s to a unit, mirrored for the right bud */
static inline struct pods_shape pods_place(const struct pods_shape *in, float box_w, float x, float y, float s, bool mirror) {
	struct pods_shape o = *in;
	if (mirror) o.x = in->kind == PODS_S_LINE ? box_w - in->x : box_w - in->x - in->w, o.w = in->kind == PODS_S_LINE ? -in->w : in->w;
	o.x = x + o.x * s, o.y = y + o.y * s, o.w *= s, o.h *= s, o.r *= s;
	return o;
}

/* The charging bolt, in a ring of radius 1 at the origin: x, y pairs, closed. */
static const float PODS_BOLT[][2] = {
	{ 0.12f, -0.52f }, { -0.30f, 0.07f }, { -0.03f, 0.07f }, { -0.12f, 0.52f }, { 0.30f, -0.07f }, { 0.03f, -0.07f },
};
#define PODS_N_BOLT 6

/* ---- where it all goes ---- */
enum pods_art { PODS_ART_BUD_L, PODS_ART_BUD_R, PODS_ART_CASE };

struct pods_hero {
	int n_art;
	struct { int art; float x, y, s; } art[3];	/* each drawing's box: top left, scale */
	int n_rings;
	struct {
		struct pods_ring ring;
		float cx, cy, r;	/* the ring; width PODS_RING_WIDTH * r */
		float label_y;		/* the top of its line: badge, then text, centred on cx */
	} rings[3];
	float w, h;		/* all of it */
	float label_h;		/* the line's height, as is the badge's */
};
#define PODS_RING_WIDTH 0.17f

/* Lays the rings and their drawings out in a box avail wide at the origin, centred, at
 * most unit pixels to a unit, smaller when it doesn't fit. (Ear tips, pro_only, are the
 * drawer's to leave out.) */
static inline void pods_hero_layout(struct pods_hero *h, const struct pods_ring *rings, int n, float avail, float unit) {
	const float ART_H = PODS_CASE_H, GAP = 22, CASE_GAP = 40, RING = 50, RING_GAP = 18, LABEL = 24, LABEL_GAP = 10;
	const float BUD_COL = 96, PAIR_W = PODS_BUD_W * 2 - 10;	/* a pair of buds overlaps a little */
	bool art = !(n == 1 && rings[0].parts == PODS_PART_HEADSET);
	float col[3], total = 0;
	for (int i = 0; i < n; i++) {
		int p = rings[i].parts;
		col[i] = p == PODS_PART_CASE ? PODS_CASE_W : p == (PODS_PART_LEFT | PODS_PART_RIGHT) ? PAIR_W + 16 : BUD_COL;
		if (!art) col[i] = 140;
		total += col[i] + (i ? (p == PODS_PART_CASE ? CASE_GAP : GAP) : 0);
	}
	float s = unit;
	if (total * s > avail) s = avail / total;
	h->n_art = h->n_rings = 0;
	h->w = total * s;
	float x = (avail - h->w) / 2, top = art ? ART_H + RING_GAP : 0;
	for (int i = 0; i < n; i++) {
		int p = rings[i].parts;
		if (i) x += (p == PODS_PART_CASE ? CASE_GAP : GAP) * s;
		float cx = x + col[i] * s / 2;
		if (art && p == PODS_PART_CASE) {
			h->art[h->n_art].art = PODS_ART_CASE, h->art[h->n_art].x = x, h->art[h->n_art].y = 0, h->art[h->n_art++].s = s;
		} else if (art) {
			float bx = cx - (p == (PODS_PART_LEFT | PODS_PART_RIGHT) ? PAIR_W : PODS_BUD_W) * s / 2;
			float by = (ART_H - PODS_BUD_H) / 2 * s;
			if (p & PODS_PART_LEFT) {
				h->art[h->n_art].art = PODS_ART_BUD_L, h->art[h->n_art].x = bx, h->art[h->n_art].y = by, h->art[h->n_art++].s = s;
				bx += (PODS_BUD_W - 10) * s;
			}
			if (p & PODS_PART_RIGHT) {
				h->art[h->n_art].art = PODS_ART_BUD_R, h->art[h->n_art].x = bx, h->art[h->n_art].y = by, h->art[h->n_art++].s = s;
			}
		}
		h->rings[h->n_rings].ring = rings[i];
		h->rings[h->n_rings].cx = cx;
		h->rings[h->n_rings].cy = (top + RING / 2) * s;
		h->rings[h->n_rings].r = RING / 2 * s * (1 - PODS_RING_WIDTH / 2);
		h->rings[h->n_rings++].label_y = (top + RING + LABEL_GAP) * s;
		x += col[i] * s;
	}
	h->label_h = LABEL * s;
	h->h = (top + RING + LABEL_GAP + LABEL) * s;
}
#endif
