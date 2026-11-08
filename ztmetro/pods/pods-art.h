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
#define PODS_C_EDGE	0x9a9aa2	/* outlines: the drawing is white on any background */
#define PODS_C_BADGE	0x8e8e93	/* the L and R discs */

/* ---- paints: a colour, or a linear gradient across the drawing's own box ---- */
struct pods_fill {
	int n;			/* stops; 1 is a plain colour */
	float x0, y0, x1, y1;	/* the gradient's line, in the box's units */
	float at[4];
	unsigned rgb[4];
};
enum {
	PODS_F_HEAD, PODS_F_STEM, PODS_F_TIP, PODS_F_SENSOR, PODS_F_METAL, PODS_F_GAP, PODS_F_SHINE,
	PODS_F_CASE, PODS_F_SEAM, PODS_F_LED_OFF, PODS_F_LED_ON,
};
static const struct pods_fill PODS_FILLS[] = {	/* in the order of the names above */
	/* the bud's head: gloss, lit from above on the outer side */
	{ 3, 10, 2, 52, 56, { 0, 0.5f, 1 }, { 0xffffff, 0xeeeef2, 0xc6c6ce } },
	/* the stem, a cylinder: dark at both edges, light just off its middle */
	{ 4, 29, 0, 47, 0, { 0, 0.32f, 0.62f, 1 }, { 0xc2c2ca, 0xfcfcfd, 0xe8e8ec, 0xa9a9b2 } },
	/* silicone: softer, greyer */
	{ 3, 0, 18, 0, 50, { 0, 0.45f, 1 }, { 0xe6e6ea, 0xcfcfd5, 0xa4a4ad } },
	{ 2, 0, 12, 0, 38, { 0, 1 }, { 0x4a4a52, 0x101013 } },
	/* the stem's end: brushed metal, a cylinder too */
	{ 4, 29, 0, 47, 0, { 0, 0.35f, 0.7f, 1 }, { 0x7c7c85, 0xe4e4e8, 0xa2a2aa, 0x66666e } },
	{ 1, 0, 0, 0, 0, { 0 }, { 0x55555c } },
	{ 2, 0, 6, 0, 30, { 0, 1 }, { 0xffffff, 0xf4f4f7 } },
	{ 3, 0, 0, 0, 108, { 0, 0.55f, 1 }, { 0xfbfbfd, 0xebebef, 0xcfcfd6 } },
	{ 1, 0, 0, 0, 0, { 0 }, { 0xb4b4bc } },
	{ 1, 0, 0, 0, 0, { 0 }, { 0xb8b8be } },
	{ 1, 0, 0, 0, 0, { 0 }, { 0x3ddc68 } },	/* the lid is open: the light is on */
};

/* ---- shapes ---- */
enum pods_shape_kind { PODS_S_ELLIPSE, PODS_S_RRECT, PODS_S_LINE, PODS_S_PATH };
enum pods_op { PODS_END, PODS_M, PODS_L, PODS_C, PODS_Z };
struct pods_seg { unsigned char op; float p[6]; };	/* M x y, L x y, C x1 y1 x2 y2 x y */

/* Ellipse and rounded rectangle: the box x, y, w, h, and r the corners' radius. Line: from
 * x, y by w, h, r wide. Path: path, in the drawing's units. edged: outlined in PODS_C_EDGE,
 * every edged shape first, then all are filled, so where two meet there is no line. */
struct pods_shape {
	unsigned char kind, fill, edged, pro_only;
	float x, y, w, h, r;
	const struct pods_seg *path;
};

/* The left bud, from the front, in a 72 x 108 box; the right is its mirror. A rounded
 * head, the ear tip behind it out to the side, a stem shaded as the round thing it is, the
 * sensor's dark window, and a metal end. */
#define PODS_BUD_W 72.0f
#define PODS_BUD_H 108.0f
static const struct pods_seg PODS_BUD_HEAD[] = {
	{ PODS_M, { 38, 3 } },
	{ PODS_C, { 52, 3, 61, 13, 60.5f, 27 } },
	{ PODS_C, { 60, 39, 53, 47, 47, 52 } },
	{ PODS_L, { 30, 52 } },
	{ PODS_C, { 22, 48, 15, 39, 15.5f, 27 } },
	{ PODS_C, { 16, 12, 25, 3, 38, 3 } },
	{ PODS_Z, { 0 } }, { PODS_END, { 0 } },
};
static const struct pods_seg PODS_BUD_STEM[] = {
	{ PODS_M, { 29.5f, 40 } }, { PODS_L, { 47.5f, 40 } }, { PODS_L, { 46.5f, 98 } },
	{ PODS_C, { 46.5f, 103.5f, 43.5f, 106, 38.5f, 106 } }, { PODS_C, { 33.5f, 106, 30.5f, 103.5f, 30.5f, 98 } },
	{ PODS_Z, { 0 } }, { PODS_END, { 0 } },
};
static const struct pods_seg PODS_BUD_END[] = {	/* the stem's last 9 units */
	{ PODS_M, { 30.7f, 96 } }, { PODS_L, { 46.3f, 96 } }, { PODS_L, { 46.5f, 98 } },
	{ PODS_C, { 46.5f, 103.5f, 43.5f, 106, 38.5f, 106 } }, { PODS_C, { 33.5f, 106, 30.5f, 103.5f, 30.5f, 98 } },
	{ PODS_Z, { 0 } }, { PODS_END, { 0 } },
};
static const struct pods_seg PODS_BUD_TIP[] = {	/* tilted a little, as it sits */
	{ PODS_M, { 2, 33 } },
	{ PODS_C, { 1, 24, 9, 18, 18, 19 } },
	{ PODS_C, { 27, 20, 31, 29, 29, 38 } },
	{ PODS_C, { 27, 46, 18, 50, 11, 48 } },
	{ PODS_C, { 5, 46, 2, 40, 2, 33 } },
	{ PODS_Z, { 0 } }, { PODS_END, { 0 } },
};
static const struct pods_shape PODS_BUD[] = {
	{ PODS_S_PATH, PODS_F_TIP, 1, 1, 0, 0, 0, 0, 0, PODS_BUD_TIP },
	{ PODS_S_PATH, PODS_F_STEM, 1, 0, 0, 0, 0, 0, 0, PODS_BUD_STEM },
	{ PODS_S_PATH, PODS_F_HEAD, 1, 0, 0, 0, 0, 0, 0, PODS_BUD_HEAD },
	{ PODS_S_ELLIPSE, PODS_F_SHINE, 0, 0, 27, 7, 16, 9, 0, NULL },	/* a soft highlight */
	{ PODS_S_RRECT, PODS_F_SENSOR, 0, 0, 34.5f, 13, 8, 23, 4, NULL },
	{ PODS_S_PATH, PODS_F_METAL, 0, 0, 0, 0, 0, 0, 0, PODS_BUD_END },
	{ PODS_S_LINE, PODS_F_GAP, 0, 0, 30.8f, 96, 15.4f, 0, 0.8f, NULL },
};
#define PODS_N_BUD ((int)(sizeof(PODS_BUD) / sizeof(PODS_BUD[0])))

/* the case, lid closed, from the front, in a 150 x 108 box */
#define PODS_CASE_W 150.0f
#define PODS_CASE_H 108.0f
static const struct pods_shape PODS_CASE[] = {
	{ PODS_S_RRECT, PODS_F_CASE, 1, 0, 2, 2, 146, 104, 34, NULL },
	{ PODS_S_LINE, PODS_F_SEAM, 0, 0, 4, 32, 142, 0, 1.2f, NULL },	/* lid */
	{ PODS_S_ELLIPSE, PODS_F_LED_OFF, 0, 0, 72.5f, 52, 5, 5, 0, NULL },
};
#define PODS_N_CASE ((int)(sizeof(PODS_CASE) / sizeof(PODS_CASE[0])))

/* From a drawing's units to the page: its box's top left at x, y, s to a unit, mirrored
 * (about the box's middle) for the right bud. */
struct pods_xf { float x, y, s, box_w; bool mirror; };
static inline float pods_xf_x(const struct pods_xf *t, float x) { return t->x + (t->mirror ? t->box_w - x : x) * t->s; }
static inline float pods_xf_y(const struct pods_xf *t, float y) { return t->y + y * t->s; }

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
	const float BUD_COL = 100, PAIR_GAP = 4, PAIR_W = PODS_BUD_W * 2 - PAIR_GAP;	/* a pair: their heads ~20 apart */
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
				bx += (PODS_BUD_W - PAIR_GAP) * s;
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
