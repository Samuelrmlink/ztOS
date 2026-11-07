// SPDX-License-Identifier: GPL-2.0-or-later
/* ztGallery's logic, on any machine: make check-gallery */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../gallery/gallery-logic.h"

#define NEAR(a, b) assert(fabs((a) - (b)) < 1e-6)

static void names(void) {
	assert(gl_is_photo_name("IMG 0001.JPG", false));
	assert(gl_is_photo_name("a.jpeg", false));
	assert(gl_is_photo_name("a b;$(rm -rf x).png", false));
	assert(gl_is_photo_name("x.gif", false));
	assert(!gl_is_photo_name("x.webp", false));
	assert(gl_is_photo_name("x.webp", true));
	assert(!gl_is_photo_name(".hidden.jpg", true));
	assert(!gl_is_photo_name("jpg", true));
	assert(!gl_is_photo_name("notes.txt", true));
	assert(!gl_is_photo_name("video.mp4", true));
	assert(!gl_is_photo_name("VID_1.bgra", true));
	assert(gl_is_scanned_dir("Screenshots"));
	assert(!gl_is_scanned_dir(".cache"));
	assert(!gl_is_scanned_dir("Thumbnails"));
}

/* a little JPEG's head: SOI, an Exif segment, a frame header */
static size_t jpeg_head(uint8_t *b, bool big_endian, int orientation, int w, int h) {
	size_t n = 0;
	b[n++] = 0xff; b[n++] = 0xd8;
	uint8_t tiff[26] = {0};
	if (big_endian) {
		memcpy(tiff, "MM\0*\0\0\0\10\0\1\1\22\0\3\0\0\0\1", 18);
		tiff[18] = 0; tiff[19] = (uint8_t)orientation;
	} else {
		memcpy(tiff, "II*\0\10\0\0\0\1\0\22\1\3\0\1\0\0\0", 18);
		tiff[18] = (uint8_t)orientation; tiff[19] = 0;
	}
	size_t seg = 2 + 6 + sizeof(tiff);
	b[n++] = 0xff; b[n++] = 0xe1; b[n++] = (uint8_t)(seg >> 8); b[n++] = (uint8_t)seg;
	memcpy(b + n, "Exif\0\0", 6); n += 6;
	memcpy(b + n, tiff, sizeof(tiff)); n += sizeof(tiff);
	uint8_t sof[] = { 0xff, 0xc0, 0, 11, 8, (uint8_t)(h >> 8), (uint8_t)h, (uint8_t)(w >> 8), (uint8_t)w, 1, 1, 0x11, 0 };
	memcpy(b + n, sof, sizeof(sof)); n += sizeof(sof);
	return n;
}

static void probing(void) {
	uint8_t b[256];
	for (int o = 1; o <= 8; o++) {
		for (int be = 0; be < 2; be++) {
			size_t n = jpeg_head(b, be, o, 4032, 3024);
			struct gl_info i = gl_probe(b, n);
			assert(i.format == GL_JPEG && i.w == 4032 && i.h == 3024 && i.orientation == o);
		}
	}
	/* nonsense orientation: as if none */
	size_t n = jpeg_head(b, false, 9, 10, 20);
	struct gl_info i = gl_probe(b, n);
	assert(i.orientation == 1 && i.w == 10 && i.h == 20);
	/* every truncation of it stays inside the buffer and does not crash */
	n = jpeg_head(b, false, 6, 10, 20);
	for (size_t k = 0; k <= n; k++) (void)gl_probe(b, k);
	/* an IFD offset far past the end */
	uint8_t tiff[] = "II*\0\xff\xff\xff\xff";
	assert(gl_tiff_orientation(tiff, 8) == 0);
	/* a count that lies */
	uint8_t tiff2[] = "II*\0\10\0\0\0\xff\xff";
	assert(gl_tiff_orientation(tiff2, 10) == 0);
	/* a zero-length segment must not loop for ever */
	uint8_t bad[] = { 0xff, 0xd8, 0xff, 0xe0, 0, 0, 0xff, 0xc0 };
	assert(gl_probe(bad, sizeof(bad)).w == 0);

	uint8_t png[24] = "\x89PNG\r\n\x1a\n\0\0\0\rIHDR";
	png[16] = 0; png[17] = 0; png[18] = 0x27; png[19] = 0x10;	/* 10000 */
	png[20] = 0; png[21] = 0; png[22] = 0x0b; png[23] = 0xb8;	/* 3000 */
	i = gl_probe(png, sizeof(png));
	assert(i.format == GL_PNG && i.w == 10000 && i.h == 3000);
	uint8_t gif[10] = "GIF89a\x40\x01\xf0";
	i = gl_probe(gif, sizeof(gif));
	assert(i.format == GL_GIF && i.w == 320 && i.h == 240);
	uint8_t webp[30] = "RIFF\0\0\0\0WEBPVP8X";
	webp[24] = 0x7f; webp[25] = 0x02; webp[26] = 0;		/* 640 - 1 */
	webp[27] = 0xdf; webp[28] = 0x01; webp[29] = 0;		/* 480 - 1 */
	i = gl_probe(webp, sizeof(webp));
	assert(i.format == GL_WEBP && i.w == 640 && i.h == 480);
	uint8_t junk[64];
	for (size_t k = 0; k < sizeof(junk); k++) junk[k] = (uint8_t)(k * 37);
	assert(gl_probe(junk, sizeof(junk)).format == GL_UNKNOWN);
	/* random bytes after a JPEG's signature, many times over */
	srand(7);
	for (int r = 0; r < 20000; r++) {
		uint8_t z[128] = { 0xff, 0xd8, 0xff };
		for (size_t k = 3; k < sizeof(z); k++) z[k] = (uint8_t)rand();
		(void)gl_probe(z, sizeof(z));
		(void)gl_tiff_orientation(z, sizeof(z));
	}
}

static void orientation(void) {
	/* a 4 x 2 image's top left corner, under each orientation */
	struct { int o, x, y; } want[] = {
		{1, 0, 0}, {2, 3, 0}, {3, 3, 1}, {4, 0, 1}, {5, 0, 0}, {6, 1, 0}, {7, 1, 3}, {8, 0, 3},
	};
	for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		int nx, ny;
		gl_orient_xy(want[i].o, 0, 0, 4, 2, &nx, &ny);
		assert(nx == want[i].x && ny == want[i].y);
	}
	/* every pixel lands inside, and on its own place */
	for (int o = 1; o <= 8; o++) {
		bool seen[8][8] = {{false}};
		int w = 4, h = 3, dw = gl_orientation_swaps(o) ? h : w, dh = gl_orientation_swaps(o) ? w : h;
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				int nx, ny;
				gl_orient_xy(o, x, y, w, h, &nx, &ny);
				assert(nx >= 0 && nx < dw && ny >= 0 && ny < dh);
				assert(!seen[ny][nx]);
				seen[ny][nx] = true;
			}
		}
	}
	assert(!gl_orientation_swaps(1) && !gl_orientation_swaps(4) && gl_orientation_swaps(6) && gl_orientation_swaps(8));
}

static void sizes(void) {
	int w, h;
	gl_fit_down(4032, 3024, 1440, 2960, &w, &h);
	assert(w == 1440 && h == 1080);
	gl_fit_down(1000, 8000, 1440, 2960, &w, &h);
	assert(w == 370 && h == 2960);
	gl_fit_down(100, 50, 1440, 2960, &w, &h);
	assert(w == 100 && h == 50);
	gl_fit_down(0, 50, 1440, 2960, &w, &h);
	assert(w == 0 && h == 0);
	gl_fit_down(100000, 1, 100, 100, &w, &h);
	assert(w == 100 && h == 1);

	/* the 12 MP photo to the screen's width: 3/8 is 1512, 2/8 would be too few */
	assert(gl_jpeg_eighths_at_least(1440.0 / 4032) == 3);
	assert(gl_jpeg_eighths_at_least(0.01) == 1);
	assert(gl_jpeg_eighths_at_least(1) == 8);
	assert(gl_jpeg_eighths_at_least(5) == 8);
	assert(gl_jpeg_eighths_at_most(4096.0 / 8000) == 4);
	assert(gl_jpeg_eighths_at_most(1) == 8);
	assert(gl_jpeg_eighths_at_most(0.001) == 1);

	assert(gl_size_allowed(GL_JPEG, 20000, 20000, 500000000, 100000000));
	assert(!gl_size_allowed(GL_PNG, 20000, 20000, 500000000, 100000000));
	assert(gl_size_allowed(GL_PNG, 10000, 10000, 500000000, 100000000));
	assert(gl_size_allowed(GL_GIF, 0, 0, 1, 1));
	assert(!gl_size_allowed(GL_JPEG, 65535, 65535, 500000000, 100000000));
}

static void grid(void) {
	struct gl_grid g = gl_grid_for(360, 4, 16, 8, 4, 4);
	assert(g.cell == 81 && g.row_h == 85);
	/* it ends inside the width */
	assert(gl_grid_x(&g, 3) + g.cell <= 360 - 8);
	assert(gl_grid_rows(&g, 0) == 0 && gl_grid_rows(&g, 1) == 1 && gl_grid_rows(&g, 4) == 1 && gl_grid_rows(&g, 5) == 2);
	assert(gl_grid_col(&g, 0) == 0);
	assert(gl_grid_col(&g, 16) == 0);
	assert(gl_grid_col(&g, 96) == 0);
	assert(gl_grid_col(&g, 102) == 1);
	assert(gl_grid_col(&g, 359) == 3);
	int a, b;
	assert(gl_rows_in(&g, 100, 108, 104, 740, &a, &b));
	assert(a == 0 && b == 7);
	assert(gl_rows_in(&g, 100, 108, 108 + 85 * 10 + 1, 108 + 85 * 11, &a, &b));
	assert(a == 10 && b == 10);
	assert(!gl_rows_in(&g, 0, 108, 0, 700, &a, &b));
	assert(!gl_rows_in(&g, 2, 108, 2000, 2700, &a, &b));
	/* pulled down past the top */
	assert(gl_rows_in(&g, 3, 108, -50, 600, &a, &b) && a == 0 && b == 2);
}

static void zoom(void) {
	double fw, fh, vw = 360, vh = 680;
	zv_fit(vw, vh, 4032, 3024, &fw, &fh);
	NEAR(fw, 360); NEAR(fh, 270);
	zv_fit(vw, vh, 1000, 8000, &fw, &fh);
	NEAR(fh, 680); NEAR(fw, 85);
	zv_fit(vw, vh, 0, 0, &fw, &fh);
	NEAR(fw, vw);

	/* a 12 MP photo at scale 4: 2.8 of its pixels a pixel at zoom 1 */
	zv_fit(vw, vh, 4032, 3024, &fw, &fh);
	NEAR(zv_max_zoom(fw, 4, 4032), 5.6);
	NEAR(zv_max_zoom(fw, 4, 200), ZV_MIN_MAX);
	NEAR(zv_max_zoom(fw, 4, 40000), 12);

	/* at zoom 1 a landscape photo cannot be moved at all */
	struct zv s = zv_clamped((struct zv){ 1, 50, 50 }, vw, vh, fw, fh, 6);
	NEAR(s.x, 0); NEAR(s.y, 0);
	/* zoomed in: only to the photo's edges */
	s = zv_clamped((struct zv){ 2, 1000, 1000 }, vw, vh, fw, fh, 6);
	NEAR(s.x, 180); NEAR(s.y, 0);
	s = zv_clamped((struct zv){ 4, -1000, -1000 }, vw, vh, fw, fh, 6);
	NEAR(s.x, -540); NEAR(s.y, -(270 * 4 - 680) / 2.0);
	s = zv_clamped((struct zv){ 99, 0, 0 }, vw, vh, fw, fh, 6);
	NEAR(s.zoom, 6);
	s = zv_clamped((struct zv){ 0.2, 9, 9 }, vw, vh, fw, fh, 6);
	NEAR(s.zoom, 1); NEAR(s.x, 0);

	/* a pinch keeps the photo's point under the fingers */
	struct zv a = { 1, 0, 0 };
	struct zv p = zv_pinch(a, 100, 200, 50, 20, 50, 20, 6);
	NEAR(p.zoom, 2);
	NEAR((50 - p.x) / p.zoom, (50 - a.x) / a.zoom);
	NEAR((20 - p.y) / p.zoom, (20 - a.y) / a.zoom);
	/* and follows them when they travel together */
	p = zv_pinch(a, 100, 100, 50, 20, 80, -10, 6);
	NEAR(p.zoom, 1); NEAR(p.x, 30); NEAR(p.y, -30);
	/* from where it already was */
	struct zv b = { 2, 30, -40 };
	p = zv_pinch(b, 100, 150, 10, 10, 10, 10, 6);
	NEAR(p.zoom, 3);
	NEAR((10 - p.x) / p.zoom, (10 - b.x) / b.zoom);
	/* past the ends it gives, but only a little */
	p = zv_pinch(a, 200, 100, 0, 0, 0, 0, 6);
	assert(p.zoom < 1 && p.zoom >= ZV_RUBBER_MIN);
	p = zv_pinch(a, 10, 2000, 0, 0, 0, 0, 6);
	assert(p.zoom > 6);
	p = zv_pinch(a, 0, 100, 0, 0, 0, 0, 6);		/* fingers on the same spot */
	assert(isfinite(p.zoom) && isfinite(p.x));

	/* a double tap goes in on the point, and out again */
	s = zv_double_tap(a, 100, 50, vw, vh, fw, fh, 2.5, 6);
	NEAR(s.zoom, 2.5);
	assert(s.x < 0);
	s = zv_double_tap(s, 0, 0, vw, vh, fw, fh, 2.5, 6);
	NEAR(s.zoom, 1); NEAR(s.x, 0);
	s = zv_double_tap(a, 0, 0, vw, vh, fw, fh, 8, 3);
	NEAR(s.zoom, 3);

	/* a swipe */
	assert(zv_swipe_result(-200, 0, vw, true, true) == 1);
	assert(zv_swipe_result(200, 0, vw, true, true) == -1);
	assert(zv_swipe_result(-50, 0, vw, true, true) == 0);
	assert(zv_swipe_result(-50, -1, vw, true, true) == 1);
	assert(zv_swipe_result(50, 1, vw, true, true) == -1);
	assert(zv_swipe_result(-200, 0, vw, true, false) == 0);
	assert(zv_swipe_result(200, 0, vw, false, true) == 0);
	/* thrown back against the way it was dragged: it stays */
	assert(zv_swipe_result(50, -1, vw, true, true) == 0);

	NEAR(gl_ease_out(0), 0); NEAR(gl_ease_out(1), 1); NEAR(gl_ease_out(2), 1);
	assert(gl_ease_out(0.5) > 0.5);
}

static void picking(void) {
	char out[64];
	assert(gl_pick_line("/a b/c.jpg", false, out, sizeof(out)) == 11);
	assert(!memcmp(out, "/a b/c.jpg\n", 11));
	assert(gl_pick_line("/a\nb.jpg", true, out, sizeof(out)) == 9);
	assert(!memcmp(out, "/a\nb.jpg\0", 9));
	assert(gl_pick_line("/x", false, out, 2) == 0);
}

int main(void) {
	names();
	probing();
	orientation();
	sizes();
	grid();
	zoom();
	picking();
	puts("gallery: ok");
	return 0;
}
