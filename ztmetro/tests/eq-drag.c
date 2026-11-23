// SPDX-License-Identifier: GPL-2.0-or-later
/* The equalizer's bands as vertical sliders (sound/eq-drag.h, ROADMAP 8.67). */
#include <assert.h>
#include <stdio.h>
#include "../sound/eq-drag.h"

static int near(double a, double b) { return fabs(a - b) < 1e-9; }

int main(void) {
	/* the axis is fixed once the finger has gone the slop, by the larger move */
	assert(eq_drag_axis_of(0, 0) == EQ_AXIS_UNDECIDED);
	assert(eq_drag_axis_of(5, -7) == EQ_AXIS_UNDECIDED);
	assert(eq_drag_axis_of(2, 9) == EQ_AXIS_GAIN);
	assert(eq_drag_axis_of(-3, -12) == EQ_AXIS_GAIN);
	assert(eq_drag_axis_of(9, 9) == EQ_AXIS_GAIN);
	assert(eq_drag_axis_of(12, 3) == EQ_AXIS_NONE);
	assert(eq_drag_axis_of(-40, 20) == EQ_AXIS_NONE);

	/* the band is the nearest column, whatever the height */
	double cols[10] = { 50, 80, 110, 140, 170, 200, 230, 260, 290, 320 };
	assert(eq_band_at(cols, 10, 0) == 0);
	assert(eq_band_at(cols, 10, 94) == 1);
	assert(eq_band_at(cols, 10, 96) == 2);
	assert(eq_band_at(cols, 10, 400) == 9);

	/* gain is relative to where it was: a touch alone changes nothing */
	assert(near(eq_drag_gain(6, 100, 100, 115, 15), 6));
	/* up the plot's half height is +range */
	assert(near(eq_drag_gain(0, 200, 85, 115, 15), 15));
	assert(near(eq_drag_gain(0, 200, 315, 115, 15), -15));
	/* clamped, and snapped to 0 near it */
	assert(near(eq_drag_gain(10, 200, 0, 115, 15), 15));
	assert(near(eq_drag_gain(0.3, 100, 100.5, 115, 15), 0));

	/* a whole gesture: sideways first moves nothing, later vertical motion in
	 * the same gesture does not either (the axis stays NONE) */
	enum eq_drag_axis ax = EQ_AXIS_UNDECIDED;
	double moves[][2] = { { 3, 1 }, { 10, 2 }, { 12, 40 } };
	for (int i = 0; i < 3; i++)
		if (ax == EQ_AXIS_UNDECIDED) ax = eq_drag_axis_of(moves[i][0], moves[i][1]);
	assert(ax == EQ_AXIS_NONE);
	printf("eq-drag: ok\n");
	return 0;
}
