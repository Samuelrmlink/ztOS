/* rotation.h on the host: the clip turn, the touch mapping and the scissor
 * box agree with each other for every quarter turn, and the policy keeps
 * the lock screen, calls, the AOD and opted-out apps portrait. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../rotation.h"

#define PW 1440
#define PH 2960

static bool near(double a, double b) {
	return fabs(a - b) < 1e-9;
}

/* a logical point (0..1) through the clip turn lands where rot_untouch says,
 * and a touch there comes back as the same logical point */
static void test_points(void) {
	static const double pts[][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0.25, 0.75 }, { 0.9, 0.1 } };
	for (int r = 0; r < 4; r++) {
		float m[16];
		rot_clip(r, m);
		for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); i++) {
			double ux = pts[i][0], uy = pts[i][1];
			double cx = 2 * ux - 1, cy = 2 * uy - 1;
			double qx = m[0] * cx + m[4] * cy, qy = m[1] * cx + m[5] * cy;
			double nx, ny, bx, by;
			rot_untouch(r, ux, uy, &nx, &ny);
			assert(near((qx + 1) / 2, nx) && near((qy + 1) / 2, ny));
			rot_touch(r, nx, ny, &bx, &by);
			assert(near(bx, ux) && near(by, uy));
		}
	}
	/* turned left (r 1): the logical top left is the panel's top right */
	double nx, ny;
	rot_untouch(1, 0, 0, &nx, &ny);
	assert(nx == 1 && ny == 0);
	rot_untouch(3, 0, 0, &nx, &ny);
	assert(nx == 0 && ny == 1);
}

/* a scissor box covers exactly the panel pixels its logical pixels land on */
static void test_boxes(void) {
	for (int r = 0; r < 4; r++) {
		int lw = r & 1 ? PH : PW, lh = r & 1 ? PW : PH;
		int x = 100, y = 40, w = 300, h = 96, out[4];
		rot_box(r, PW, PH, x, y, w, h, out);
		assert(out[0] >= 0 && out[1] >= 0 && out[0] + out[2] <= PW && out[1] + out[3] <= PH);
		/* the corners' pixel centres map inside the box, and the area is kept */
		assert(out[2] * out[3] == w * h);
		static const double corner[][2] = { { 0.5, 0.5 }, { -0.5, 0.5 }, { 0.5, -0.5 }, { -0.5, -0.5 } };
		for (int c = 0; c < 4; c++) {
			double px = c & 1 ? x + w + corner[c][0] : x + corner[c][0];
			double py = c & 2 ? y + h + corner[c][1] : y + corner[c][1];
			double nx, ny;
			rot_untouch(r, px / lw, py / lh, &nx, &ny);
			nx *= PW, ny *= PH;
			assert(nx > out[0] && nx < out[0] + out[2] && ny > out[1] && ny < out[1] + out[3]);
		}
		/* the whole logical screen is the whole panel */
		rot_box(r, PW, PH, 0, 0, lw, lh, out);
		assert(out[0] == 0 && out[1] == 0 && out[2] == PW && out[3] == PH);
	}
}

static void test_policy(void) {
	struct rot_ctx c = { .enabled = true, .sensor = 1, .screen_on = true };
	assert(rot_wanted(&c) == 1 && rot_sensor_wanted(&c));
	c.locked = true;
	assert(rot_wanted(&c) == 0 && rot_sensor_wanted(&c));	/* measured, ready to unlock */
	c.locked = false;
	c.call_screen = true;
	assert(rot_wanted(&c) == 0);
	c.call_screen = false;
	c.app_opts_out = true;
	assert(rot_wanted(&c) == 0);
	c.app_opts_out = false;
	c.lock_tile = true;
	assert(rot_wanted(&c) == 0 && !rot_sensor_wanted(&c));
	c.lock_tile = false;
	c.screen_on = false;
	assert(rot_wanted(&c) == 0 && !rot_sensor_wanted(&c));
	c.screen_on = true;
	c.aod = true;
	assert(rot_wanted(&c) == 0 && !rot_sensor_wanted(&c));
	c.aod = false;
	c.enabled = false;
	assert(rot_wanted(&c) == 0 && !rot_sensor_wanted(&c));
	c.enabled = true;
	c.sensor = -1;
	assert(rot_wanted(&c) == 0);
	c.sensor = 3;
	assert(rot_wanted(&c) == 3);
	assert(rot_app_opts_out("ztcamera") && rot_app_opts_out("waydroid.com.android.chrome") &&
		!rot_app_opts_out("ztsettings") && !rot_app_opts_out(NULL));
	assert(rot_units(0, 6, 8) == 6 && rot_units(1, 4, 8) == 8 && rot_units(3, 6, 8) == 8 &&
		rot_units(2, 4, 8) == 4);
}

int main(void) {
	test_points();
	test_boxes();
	test_policy();
	puts("rotation: PASS");
	return 0;
}
