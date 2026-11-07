// SPDX-License-Identifier: GPL-3.0-or-later
/* The status bar's AirPods icon, its own icon beside Bluetooth's: both buds with a thin
 * meter either side, each its own pod's, or, with only one pod out of the case, that pod with a
 * tiny L or R by its stem and its meter to the right. Fed by ztos-podsd, not BlueZ. */
#ifndef PODS_BAR_H
#define PODS_BAR_H

#include <cairo.h>
#include <math.h>

#include "pods-bar-state.h"

/* A bud with its stem hanging down: a head and a bar. mirror -1 for the left one. */
static inline void pods_bar_bud(cairo_t *cr, double x, double base, double h, double mirror) {
	double head = h * 0.27, stem = h * 0.17;
	cairo_arc(cr, x + mirror * head * 0.25, base - h + head, head, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_set_line_width(cr, stem);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_move_to(cr, x - mirror * head * 0.45, base - h + head * 1.3);
	cairo_line_to(cr, x - mirror * head * 0.45, base - stem / 2);
	cairo_stroke(cr);
}

/* One pod's meter, filling from the bottom. Nothing for a level not known. */
static inline void pods_bar_meter(cairo_t *cr, double x, double base, double h, double w, int level, bool charging) {
	if (level < 0 || level > 100) return;
	double fill = h * level / 100.0;
	if (level > 0 && fill < 1.0) fill = 1.0;	/* some charge is never an empty bar */
	cairo_set_source_rgba(cr, 1, 1, 1, 0.28);
	cairo_rectangle(cr, x, base - h, w, h);
	cairo_fill(cr);
	if (level <= 15 && !charging) cairo_set_source_rgb(cr, 0.9, 0.16, 0.16);
	else cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_rectangle(cr, x, base - fill, w, fill);
	cairo_fill(cr);
}

/* From x, on the bar's baseline, not past `limit`. */
static inline void pods_bar_draw(cairo_t *cr, const struct pods_bar *b, double x, double base, double cell, double limit) {
	double h = cell * 1.05, gap = h * 0.5, bar_w = cell * 0.2, space = cell * 0.22;
	cairo_save(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	if (b->single >= 0) {
		bool left = b->single == 0;
		char letter[2] = { left ? 'L' : 'R', 0 };
		double size = cell * 0.52;
		cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cr, size);
		cairo_text_extents_t ext;
		cairo_text_extents(cr, letter, &ext);
		double end = x + gap + ext.x_advance + space + bar_w;
		if (end <= limit) {
			/* each by its stem: a left pod's stem is on its right, a right pod's on its left */
			double bud = left ? x + gap * 0.5 : x + ext.x_advance + gap * 0.5;
			pods_bar_bud(cr, bud, base, h, left ? -1 : 1);
			cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_move_to(cr, left ? x + gap : x, base);
			cairo_show_text(cr, letter);
			pods_bar_meter(cr, end - bar_w, base, h, bar_w, left ? b->left : b->right,
				left ? b->charge_left : b->charge_right);
		}
	} else {
		double end = x + bar_w + space + gap * 2 + space + bar_w;
		if (end <= limit) {	/* left out rather than run into the battery */
			double buds = x + bar_w + space;
			pods_bar_meter(cr, x, base, h, bar_w, b->left, b->charge_left);
			cairo_set_source_rgb(cr, 1, 1, 1);
			pods_bar_bud(cr, buds + gap * 0.5, base, h, -1);
			pods_bar_bud(cr, buds + gap * 1.5, base, h, 1);
			pods_bar_meter(cr, end - bar_w, base, h, bar_w, b->right, b->charge_right);
		}
	}
	cairo_restore(cr);
}

#endif
