/*
 * Auto-rotation of the phone's own panel (ROADMAP 8.33): the arithmetic,
 * kept apart from ztmetro.c so the host test (tests/test-rotation.c) can
 * check it without wlroots or GL.
 *
 * The rotation r is ztoscsf-sensord's: how far the phone is turned from
 * portrait, in quarter turns counter-clockwise as you look at the glass.
 * 1 is the earpiece on the left (the right-hand edge up), 3 the earpiece on
 * the right, 2 upside down. The shell draws everything in "logical" points,
 * with (0, 0) at the top left as the user now sees it; the panel itself
 * stays portrait. Turned (r odd), the logical width is the panel's height.
 *
 * Mapping a logical point (lx, ly) to the panel (px, py), with the panel's
 * width PW and height PH in the same units:
 *	r 0: (lx, ly)
 *	r 1: (PW - ly, lx)		the logical top is the panel's right edge
 *	r 2: (PW - lx, PH - ly)
 *	r 3: (ly, PH - lx)		the logical top is the panel's left edge
 *
 * Nothing here decides policy beyond rot_wanted: which screens turn.
 */
#ifndef ZT_ROTATION_H
#define ZT_ROTATION_H

#include <stdbool.h>
#include <string.h>

/* What decides whether the panel turns now. */
struct rot_ctx {
	bool enabled;		/* Settings > auto-rotate (config auto_rotate) */
	bool lock_tile;		/* the Rotation Lock quick action is on */
	int sensor;		/* the last orientation, 0..3; -1 none yet */
	bool screen_on, aod;
	bool locked;		/* the lock screen is up, or on its way */
	bool call_screen;	/* a call's or an alarm's screen over the lock */
	bool special;		/* splash, sensor hub failure, DotMatrix: portrait screens */
	bool app_opts_out;	/* the app in front turns itself, or must not be turned */
};

/* The rotation to draw at: the sensor's, unless one of these is portrait.
 * As on stock: the lock screen and a call's screen stay portrait, and so
 * does the always-on display. */
static inline int rot_wanted(const struct rot_ctx *c) {
	if (!c->enabled || c->lock_tile || c->sensor < 0 || c->sensor > 3 || !c->screen_on ||
			c->aod || c->locked || c->call_screen || c->special || c->app_opts_out) {
		return 0;
	}
	return c->sensor;
}

/* Whether the orientation is worth measuring: only while it could be used.
 * (While locked it is, so that the first screen after unlocking is right.) */
static inline bool rot_sensor_wanted(const struct rot_ctx *c) {
	return c->enabled && !c->lock_tile && c->screen_on && !c->aod;
}

/* Apps that turn themselves (the camera, from its own reading of the
 * sensor) or that must be left alone (Waydroid's Android, which has its own
 * rotation and is not resized): the panel stays portrait while they are in
 * front. */
static inline bool rot_app_opts_out(const char *app_id) {
	if (!app_id) {
		return false;
	}
	return !strcmp(app_id, "ztcamera") || !strncmp(app_id, "waydroid", 8) ||
		!strcmp(app_id, "Waydroid");
}

/* The clip-space turn applied after the projection, column-major as GL
 * wants: (cx, cy) as drawn for the logical screen to where it lands on the
 * panel. In clip space both run -1..1, so a quarter turn is exact whatever
 * the aspect, and the perspective's w is untouched. */
static inline void rot_clip(int r, float m[16]) {
	memset(m, 0, 16 * sizeof(float));
	m[10] = m[15] = 1.0f;
	switch (r & 3) {
	case 0: m[0] = 1; m[5] = 1; break;
	case 1: m[4] = -1; m[1] = 1; break;	/* qx = -cy, qy = cx */
	case 2: m[0] = -1; m[5] = -1; break;
	case 3: m[4] = 1; m[1] = -1; break;	/* qx = cy, qy = -cx */
	}
}

/* A touch on the panel, as wlroots gives it (0..1 across and down the
 * portrait panel), to 0..1 across and down the logical screen. */
static inline void rot_touch(int r, double nx, double ny, double *ux, double *uy) {
	switch (r & 3) {
	case 0: *ux = nx; *uy = ny; break;
	case 1: *ux = ny; *uy = 1.0 - nx; break;
	case 2: *ux = 1.0 - nx; *uy = 1.0 - ny; break;
	case 3: *ux = 1.0 - ny; *uy = nx; break;
	}
}

/* The inverse, for tests and for anything placed on the panel by a
 * logical point: logical 0..1 to the panel's 0..1. */
static inline void rot_untouch(int r, double ux, double uy, double *nx, double *ny) {
	switch (r & 3) {
	case 0: *nx = ux; *ny = uy; break;
	case 1: *nx = 1.0 - uy; *ny = ux; break;
	case 2: *nx = 1.0 - ux; *ny = 1.0 - uy; break;
	case 3: *nx = uy; *ny = 1.0 - ux; break;
	}
}

/* A scissor box in logical pixels (x, y from the top left as seen, w, h) to
 * the panel's buffer, whose first row is its top: pw x ph pixels. */
static inline void rot_box(int r, int pw, int ph, int x, int y, int w, int h, int out[4]) {
	switch (r & 3) {
	case 0: out[0] = x; out[1] = y; out[2] = w; out[3] = h; break;
	case 1: out[0] = pw - y - h; out[1] = x; out[2] = h; out[3] = w; break;
	case 2: out[0] = pw - x - w; out[1] = ph - y - h; out[2] = w; out[3] = h; break;
	case 3: out[0] = y; out[1] = ph - x - w; out[2] = h; out[3] = w; break;
	}
}

/* Start's columns turned: twice as many small tiles across, as the screen
 * is about twice as wide, up to max (an even number). */
static inline int rot_units(int r, int units, int max) {
	if (!(r & 1)) {
		return units;
	}
	int u = units * 2;
	return u > max ? max : u;
}

#endif
