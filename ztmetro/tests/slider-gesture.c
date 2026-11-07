/* make check-slider-gesture: whose a finger on a slider is, a slider's value, the
 * title's one pass and where a switch's On/Off goes. Nothing is drawn. */
#include <assert.h>
#include <stdio.h>
#include "../core/zmc-gesture.h"

int main(void) {
	/* nothing is decided in the first few points, whichever way */
	assert(zmc_slider_axis(0, 0) == ZMC_AXIS_NONE);
	assert(zmc_slider_axis(8, 0) == ZMC_AXIS_NONE);
	assert(zmc_slider_axis(0, -8) == ZMC_AXIS_NONE);
	assert(zmc_slider_axis(5, 5) == ZMC_AXIS_NONE);
	/* straight up or down, and any slant steeper than 35 degrees: the page */
	assert(zmc_slider_axis(0, 9) == ZMC_AXIS_ALONG);
	assert(zmc_slider_axis(0, -30) == ZMC_AXIS_ALONG);
	assert(zmc_slider_axis(10, 10) == ZMC_AXIS_ALONG);
	assert(zmc_slider_axis(-10, 8) == ZMC_AXIS_ALONG);
	/* sideways, either way, with a thumb's slant: the slider */
	assert(zmc_slider_axis(9, 0) == ZMC_AXIS_ACROSS);
	assert(zmc_slider_axis(-9, 0) == ZMC_AXIS_ACROSS);
	assert(zmc_slider_axis(12, 6) == ZMC_AXIS_ACROSS);
	assert(zmc_slider_axis(-12, -8) == ZMC_AXIS_ACROSS);

	/* a pivot wants a flatter, longer stroke than a slider: scrolling comes first */
	assert(zmc_drag_axis(0, 0) == ZMC_AXIS_NONE);
	assert(zmc_drag_axis(11, 0) == ZMC_AXIS_NONE);
	assert(zmc_drag_axis(15, 0) == ZMC_AXIS_NONE);
	assert(zmc_drag_axis(18, 0) == ZMC_AXIS_ACROSS);
	assert(zmc_drag_axis(-30, 5) == ZMC_AXIS_ACROSS);
	assert(zmc_drag_axis(12, 6) == ZMC_AXIS_ALONG);
	assert(zmc_drag_axis(0, 12) == ZMC_AXIS_ALONG);

	/* taken near the thumb it does not jump; taken far off, the thumb comes */
	assert(zmc_slider_grab(100, 0, 200, 50) == 0);
	assert(zmc_slider_grab(120, 0, 200, 50) == 20);
	assert(zmc_slider_grab(80, 0, 200, 50) == -20);
	assert(zmc_slider_grab(180, 0, 200, 50) == 0);
	assert(zmc_slider_percent(120, 0, 200, 20) == 50);
	assert(zmc_slider_percent(121, 0, 200, 20) == 51 || zmc_slider_percent(121, 0, 200, 20) == 50);
	assert(zmc_slider_percent(180, 0, 200, 0) == 90);
	/* and it reaches both ends, past the track's */
	assert(zmc_slider_percent(-50, 0, 200, 0) == 0);
	assert(zmc_slider_percent(300, 0, 200, 0) == 100);
	assert(zmc_slider_percent(16 + 164, 16, 328, 0) == 50);

	/* a title that fits never moves */
	bool done;
	assert(zmc_marquee_offset(0, 0, &done) == 0 && done);
	assert(zmc_marquee_offset(5000, -10, &done) == 0 && done);
	/* one that does not: out over 2 s at 30 points a second, held, back in 1 s */
	assert(zmc_marquee_offset(0, 60, &done) == 0 && !done);
	assert(zmc_marquee_offset(1000, 60, &done) == 30 && !done);
	assert(zmc_marquee_offset(2000, 60, &done) == 60 && !done);
	assert(zmc_marquee_offset(2000 + ZMC_MARQUEE_HOLD_MS - 1, 60, &done) == 60 && !done);
	double back = zmc_marquee_offset(2000 + ZMC_MARQUEE_HOLD_MS + 500, 60, &done);
	assert(back == 30 && !done);
	/* and it is over: back at its start, and nothing asks for another frame */
	assert(zmc_marquee_offset(2000 + ZMC_MARQUEE_HOLD_MS + 1000, 60, &done) == 0 && done);
	assert(zmc_marquee_offset(1000000, 60, &done) == 0 && done);

	/* one line keeps On/Off where it was; a wrapped title pushes it down */
	assert(zmc_switch_label_y(28) == ZMC_SWITCH_LINE);
	assert(zmc_switch_label_y(0) == ZMC_SWITCH_LINE);
	assert(zmc_switch_label_y(56) == 56);
	assert(zmc_switch_label_y(84) == 84);
	puts("PASS: a slider's axis, value and grab; the title's pass; On/Off under a wrapped title");
	return 0;
}
