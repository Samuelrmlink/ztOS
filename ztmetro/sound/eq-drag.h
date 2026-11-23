// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The equalizer's bands as sliders (ROADMAP 8.67): each band is a vertical
 * slider for its gain, one band per gesture. Kept apart from sound-pages.c so
 * the host test (tests/eq-drag.c) runs it without a window.
 *
 * A gesture's axis is fixed the first time the finger has gone EQ_DRAG_SLOP
 * points: mostly up or down, it moves the band's gain from where it was
 * (relative, so a touch to pick a band never makes its gain jump); mostly
 * sideways, it does nothing to any band. The band is the one whose column is
 * nearest where the finger came down, and stays that band until it is lifted.
 */
#ifndef ZTMETRO_EQ_DRAG_H
#define ZTMETRO_EQ_DRAG_H

#include <math.h>

#define EQ_DRAG_SLOP 8.0

enum eq_drag_axis { EQ_AXIS_UNDECIDED, EQ_AXIS_GAIN, EQ_AXIS_NONE };

/* The axis of a gesture that has moved (dx, dy) from where it came down;
 * UNDECIDED until it has gone the slop. Exactly diagonal counts as gain. */
static inline enum eq_drag_axis eq_drag_axis_of(double dx, double dy) {
	if (fabs(dx) < EQ_DRAG_SLOP && fabs(dy) < EQ_DRAG_SLOP) return EQ_AXIS_UNDECIDED;
	return fabs(dy) >= fabs(dx) ? EQ_AXIS_GAIN : EQ_AXIS_NONE;
}

/* The band whose column (its x on the plot, cols[i]) is nearest x. */
static inline int eq_band_at(const double *cols, int n, double x) {
	int best = 0;
	for (int i = 1; i < n; i++)
		if (fabs(x - cols[i]) < fabs(x - cols[best])) best = i;
	return best;
}

/* A gain dragged from g0 by (y0 - y) points on a plot where range dB is half_h
 * points: clamped to +-range, and within 0.4 dB of 0 it is 0. */
static inline double eq_drag_gain(double g0, double y0, double y, double half_h, double range) {
	double g = g0 + (y0 - y) / half_h * range;
	g = g < -range ? -range : g > range ? range : g;
	return fabs(g) < 0.4 ? 0 : g;
}

#endif
