// SPDX-License-Identifier: GPL-3.0-or-later
/* The status bar's AirPods icon alone, into a PNG, at a few levels: to look at. On the phone:
 *   cc -std=gnu11 $(pkg-config --cflags cairo) tests/pods-bar-draw.c $(pkg-config --libs cairo) -lm -o /tmp/pb && /tmp/pb out.png */
#include "../pods/pods-bar.h"

int main(int argc, char **argv) {
	const struct pods_bar shots[] = {
		{ .link = true, .left = 87, .right = 92, .kase = -1, .single = -1 },
		{ .link = true, .left = 12, .right = 50, .kase = -1, .single = -1 },
		{ .link = true, .left = 12, .right = 50, .kase = -1, .single = -1, .charge_left = true },
		{ .link = true, .left = 100, .right = 60, .kase = 27, .single = 1, .charge_left = true },
		{ .link = true, .left = 70, .right = 100, .kase = 27, .single = 0, .charge_right = true },
		{ .link = true, .left = -1, .right = -1, .kase = -1, .single = -1 },
	};
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 840, 120);
	cairo_t *cr = cairo_create(s);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	cairo_scale(cr, 4, 4);
	for (int i = 0; i < 6; i++) {
		pods_bar_draw(cr, &shots[i], 6 + i * 34, 24, 15, 6 + (i + 1) * 34);
	}
	cairo_destroy(cr);
	int r = cairo_surface_write_to_png(s, argc > 1 ? argv[1] : "pods-bar.png");
	cairo_surface_destroy(s);
	return r != CAIRO_STATUS_SUCCESS;
}
