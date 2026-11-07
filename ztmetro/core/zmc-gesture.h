/* The arithmetic of a finger and of a title too long for its line, with nothing
 * drawn and no window: tests/slider-gesture.c checks it on any machine. */
#ifndef ZMC_GESTURE_H
#define ZMC_GESTURE_H
#include <stdbool.h>

enum { ZMC_AXIS_NONE, ZMC_AXIS_ACROSS, ZMC_AXIS_ALONG };

/* Which way a finger that came down on a slider is going, from the line between
 * where it touched and where it is. Decided once, ZMC_SLIDER_DECIDE points out:
 * nearly flat (under 35 degrees: a thumb's sideways stroke is never quite level)
 * takes the slider, and anything else is the page's. Until then nothing moves. */
#define ZMC_SLIDER_DECIDE	9.0
#define ZMC_SLIDER_TAN		0.70	/* tan 35 degrees */

static inline int zmc_slider_axis(double dx, double dy) {
	double ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
	if (ax * ax + ay * ay < ZMC_SLIDER_DECIDE * ZMC_SLIDER_DECIDE) return ZMC_AXIS_NONE;
	return ay <= ax * ZMC_SLIDER_TAN ? ZMC_AXIS_ACROSS : ZMC_AXIS_ALONG;
}

/* The same for a drag on a pivot, from anywhere but a slider. Scrolling is what a
 * thumb mostly does, and its stroke is an arc that may start at a slant, so only a
 * nearly flat stroke (within 20 degrees of the horizontal, over 18 points) is
 * across; anything else is along, decided after 12 points. */
#define ZMC_TAN_20	0.364

static inline int zmc_drag_axis(double dx, double dy) {
	double ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, len2 = ax * ax + ay * ay;
	if (len2 < 12.0 * 12.0) return ZMC_AXIS_NONE;
	if (ay > ax * ZMC_TAN_20) return ZMC_AXIS_ALONG;	/* any curve or slant: the list */
	return len2 < 18.0 * 18.0 ? ZMC_AXIS_NONE : ZMC_AXIS_ACROSS;	/* flat, and far enough to be sure */
}

/* A slider, taken at x. A finger that took it near its thumb moves the thumb from
 * where it is; anywhere else, the thumb comes to the finger. Returns the offset to
 * keep for the rest of the drag. */
#define ZMC_THUMB_REACH		28.0

static inline double zmc_slider_grab(double x, double left, double width, int percent) {
	double off = x - (left + width * percent / 100.0);
	return off >= -ZMC_THUMB_REACH && off <= ZMC_THUMB_REACH ? off : 0;
}

/* Where a slider is with the finger at x, in percent, 0..100. */
static inline int zmc_slider_percent(double x, double left, double width, double grab) {
	double p = (x - grab - left) * 100.0 / width;
	return p <= 0 ? 0 : p >= 100 ? 100 : (int)(p + 0.5);
}

/* ---- a title too long for its line ------------------------------------------- */
/* One pass and no more, so that nothing is drawn once it has shown the whole title:
 * ZMC_MARQUEE_WAIT_MS after the page arrives it moves left at ZMC_MARQUEE_SPEED
 * points a second until its end is in sight, stays ZMC_MARQUEE_HOLD_MS for reading,
 * and goes back to its start, twice as fast. */
#define ZMC_MARQUEE_WAIT_MS	1200
#define ZMC_MARQUEE_HOLD_MS	1400
#define ZMC_MARQUEE_SPEED	30.0

static inline double zmc_marquee_ease(double t) {
	return t * t * (3 - 2 * t);
}

/* How far left it is `ms` after it started to move (the wait not included), with
 * `overflow` points hidden. *done when it is back and nothing more is to be drawn. */
static inline double zmc_marquee_offset(double ms, double overflow, bool *done) {
	double out = overflow / ZMC_MARQUEE_SPEED * 1000, back = out / 2;
	*done = false;
	if (overflow <= 0 || ms >= out + ZMC_MARQUEE_HOLD_MS + back) {
		*done = true;
		return 0;
	}
	if (ms < 0) return 0;
	if (ms < out) return overflow * zmc_marquee_ease(ms / out);
	if (ms < out + ZMC_MARQUEE_HOLD_MS) return overflow;
	return overflow * (1 - zmc_marquee_ease((ms - out - ZMC_MARQUEE_HOLD_MS) / back));
}

/* A switch's On/Off under its title: where a title of `title_h` points tall puts it.
 * One line keeps the place it always had; a wrapped title pushes it under its last
 * line, and the row below down by as much. */
#define ZMC_SWITCH_LINE		26.0
#define ZMC_SWITCH_ONE_LINE	32.0

static inline double zmc_switch_label_y(double title_h) {
	return title_h > ZMC_SWITCH_ONE_LINE ? title_h : ZMC_SWITCH_LINE;
}
#endif
