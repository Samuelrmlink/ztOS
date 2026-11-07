// SPDX-License-Identifier: GPL-3.0-or-later
/* The status bar's AirPods icon alone, into a PNG, at a few levels: to look at. On the phone:
 *   cc -std=gnu11 $(pkg-config --cflags cairo) tests/pods-bar-draw.c $(pkg-config --libs cairo) -lm -o /tmp/pb && /tmp/pb out.png */
#include "../pods/pods-bar.h"

int main(int argc, char **argv) {
	const int levels[][3] = { { 87, 92, 0 }, { 12, 50, 0 }, { 12, 50, 1 }, { -1, -1, 0 } };
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 560, 120);
	cairo_t *cr = cairo_create(s);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	cairo_scale(cr, 4, 4);
	for (int i = 0; i < 4; i++) {
		struct pods_bar b = { .link = true, .left = levels[i][0], .right = levels[i][1], .kase = -1, .charging = levels[i][2] };
		pods_bar_draw(cr, &b, 6 + i * 34, 24, 15, 140);
	}
	cairo_destroy(cr);
	int r = cairo_surface_write_to_png(s, argc > 1 ? argv[1] : "pods-bar.png");
	cairo_surface_destroy(s);
	return r != CAIRO_STATUS_SUCCESS;
}
