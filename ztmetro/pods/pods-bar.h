// SPDX-License-Identifier: GPL-3.0-or-later
/* The status bar's AirPods icon: two earbuds, and beside them a thin meter of the emptier
 * pod, as the plain Bluetooth meter draws one. Fed by ztos-podsd, not by BlueZ's battery. */
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

/* From x, on the bar's baseline, not past `limit`. */
static inline void pods_bar_draw(cairo_t *cr, const struct pods_bar *b, double x, double base, double cell, double limit) {
	double h = cell * 1.05, gap = h * 0.5, bar_w = cell * 0.2;
	double end = x + gap * 2 + cell * 0.1 + bar_w;
	if (end > limit) return;	/* left out rather than run into the battery */
	cairo_save(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	pods_bar_bud(cr, x + gap * 0.5, base, h, -1);
	pods_bar_bud(cr, x + gap * 1.5, base, h, 1);
	int level = pods_bar_level(b);
	if (level >= 0 && level <= 100) {
		double bar_x = x + gap * 2 + cell * 0.1;
		double fill = h * level / 100.0;
		if (level > 0 && fill < 1.0) fill = 1.0;	/* some charge is never an empty bar */
		cairo_set_source_rgba(cr, 1, 1, 1, 0.28);
		cairo_rectangle(cr, bar_x, base - h, bar_w, h);
		cairo_fill(cr);
		if (level <= 15 && !b->charging) cairo_set_source_rgb(cr, 0.9, 0.16, 0.16);
		else cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_rectangle(cr, bar_x, base - fill, bar_w, fill);
		cairo_fill(cr);
	}
	cairo_restore(cr);
}

#endif
