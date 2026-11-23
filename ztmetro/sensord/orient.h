/*
 * The phone's orientation from the accelerometer, for auto-rotation
 * (ROADMAP 8.33). Pure: no I/O, so the host test can drive it.
 *
 * The result is how far the phone has been turned from portrait, in quarter
 * turns counter-clockwise as you look at the screen:
 *
 *	0  portrait, the earpiece at the top
 *	1  turned left: the earpiece on the left, the right-hand edge up
 *	2  upside down (off unless allow_180; stock leaves it out too)
 *	3  turned right: the earpiece on the right
 *
 * Readings are in the hub's units (about 4096 per g) and, once mapped
 * (axes), in Android's frame: x to the right of the screen, y to its top,
 * z out of the glass. Gravity read upright is +y, so the angle of the
 * reading round z, atan2(x, y), is the turn.
 *
 * What keeps it from flapping, as Android's WindowOrientationListener does
 * in simpler form:
 *  - a reading far from 1 g (shaking, walking fast) is no evidence;
 *  - the phone near flat (more than FLAT_DEG from upright) is none either:
 *    the last orientation stays;
 *  - a new orientation must be within SNAP_DEG of its own centre, so the
 *    45-degree boundary has 15 degrees of hysteresis either side;
 *  - and it must hold for SETTLE_NS before it is taken.
 * The first orientation after the sensor comes on is taken at once.
 */
#ifndef ZT_ORIENT_H
#define ZT_ORIENT_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ORIENT_G	4096.0
#define ORIENT_FLAT_DEG	65.0	/* tilt from upright beyond which the phone is "flat" */
#define ORIENT_SNAP_DEG	30.0	/* how near a new orientation's centre the reading must be */
#define ORIENT_SETTLE_NS 300000000ULL

struct orient {
	int cur;		/* taken: 0..3, -1 none since the sensor came on */
	int cand;		/* the one being timed, -1 none */
	uint64_t cand_since;
	bool allow_180;
	/* the device's x, y and z from the raw reading: axis index, sign */
	int axis[3], sign[3];
};

static inline void orient_reset(struct orient *o) {
	o->cur = -1;
	o->cand = -1;
	o->cand_since = 0;
}

static inline void orient_init(struct orient *o) {
	memset(o, 0, sizeof(*o));
	for (int i = 0; i < 3; i++) {
		o->axis[i] = i;
		o->sign[i] = 1;
	}
	orient_reset(o);
}

/* "+x+y+z" (the default), "-y+x+z" and so on: which raw axis, with which
 * sign, is the screen's right, its top and out of the glass. False if it is
 * not three distinct signed axes; o is then unchanged. */
static inline bool orient_axes(struct orient *o, const char *text) {
	int axis[3], sign[3];
	bool used[3] = { false, false, false };
	for (int i = 0; i < 3; i++) {
		char s = text[2 * i], a = text[2 * i] ? text[2 * i + 1] : 0;
		if ((s != '+' && s != '-') || a < 'x' || a > 'z' || used[a - 'x']) {
			return false;
		}
		used[a - 'x'] = true;
		axis[i] = a - 'x';
		sign[i] = s == '-' ? -1 : 1;
	}
	if (text[6] && text[6] != '\n' && text[6] != ' ') {
		return false;
	}
	memcpy(o->axis, axis, sizeof(axis));
	memcpy(o->sign, sign, sizeof(sign));
	return true;
}

static inline void orient_axes_text(const struct orient *o, char out[8]) {
	for (int i = 0; i < 3; i++) {
		out[2 * i] = o->sign[i] < 0 ? '-' : '+';
		out[2 * i + 1] = (char)('x' + o->axis[i]);
	}
	out[6] = 0;
}

/* One reading. Returns the orientation when it changes (0..3), else -1. */
static inline int orient_feed(struct orient *o, const int16_t raw[3], uint64_t now) {
	double v[3];
	for (int i = 0; i < 3; i++) {
		v[i] = o->sign[i] * (double)raw[o->axis[i]];
	}
	double mag = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (mag < 0.6 * ORIENT_G || mag > 1.4 * ORIENT_G) {
		o->cand = -1;	/* moving: wait for it to settle */
		return -1;
	}
	double tilt = asin(fabs(v[2]) / mag) * 180.0 / M_PI;
	if (tilt > ORIENT_FLAT_DEG) {
		o->cand = -1;	/* flat on a table: no evidence, nothing changes */
		return -1;
	}
	double angle = atan2(v[0], v[1]) * 180.0 / M_PI;
	if (angle < 0) {
		angle += 360.0;
	}
	int q = (int)lround(angle / 90.0) % 4;
	double off = fabs(angle - q * 90.0);
	if (off > 180.0) {
		off = 360.0 - off;
	}
	if (q == o->cur) {
		o->cand = -1;	/* still the same: any other candidate is forgotten */
		return -1;
	}
	if ((q == 2 && !o->allow_180) || (o->cur >= 0 && off > ORIENT_SNAP_DEG)) {
		o->cand = -1;	/* near a boundary, or upside down: keep what it is */
		return -1;
	}
	if (o->cur < 0) {
		o->cur = q;	/* the first since it came on: at once */
		o->cand = -1;
		return q;
	}
	if (q != o->cand) {
		o->cand = q;
		o->cand_since = now;
		return -1;
	}
	if (now - o->cand_since < ORIENT_SETTLE_NS) {
		return -1;
	}
	o->cur = q;
	o->cand = -1;
	return q;
}

#endif
