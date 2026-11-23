/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Start's photo blurred once, on the CPU, for the frosted tiles and the
 * blurred background (appearance.h). The photo does not move under the tiles
 * (only its window onto it does), so its blur is made when the photo or the
 * strength changes and then drawn as a texture like the sharp one: nothing is
 * blurred per frame, and the GPU draws no more than it did. Made on a small
 * copy (about 180 pixels across, WALL_BLUR_W) and stretched by the GPU's
 * filtering, which a blur hides.
 *
 * Three box blurs each way come close to a Gaussian. Premultiplied ARGB32, as
 * cairo keeps it; each channel the same way, so alpha stays premultiplied.
 * No cairo or GL here: tests/wall-blur.c.
 */
#ifndef ZT_WALL_BLUR_H
#define ZT_WALL_BLUR_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WALL_BLUR_W	180	/* the small copy's width, pixels */

/* One box of radius r along a run of n pixels, `step` apart, src to dst
 * (never the same memory). The edges are held: a pixel past the end is the
 * last one. */
static inline void wall_box_run(uint32_t *dst, const uint32_t *src, int n, int step, int r) {
	if (n <= 0) return;
	uint32_t sum[4] = { 0, 0, 0, 0 };
	int span = 2 * r + 1;
	/* the window centred on pixel 0: r copies of it on the left */
	for (int i = -r; i <= r; i++) {
		uint32_t p = src[(size_t)(i < 0 ? 0 : i >= n ? n - 1 : i) * step];
		for (int c = 0; c < 4; c++) sum[c] += (p >> (8 * c)) & 255;
	}
	for (int i = 0; i < n; i++) {
		uint32_t out = 0;
		for (int c = 0; c < 4; c++) out |= ((sum[c] + (uint32_t)span / 2) / (uint32_t)span) << (8 * c);
		dst[(size_t)i * step] = out;
		int add = i + r + 1, drop = i - r;
		uint32_t pa = src[(size_t)(add >= n ? n - 1 : add) * step];
		uint32_t pd = src[(size_t)(drop < 0 ? 0 : drop) * step];
		for (int c = 0; c < 4; c++) sum[c] += ((pa >> (8 * c)) & 255) - ((pd >> (8 * c)) & 255);
	}
}

/* The picture (w x h pixels, `stride` pixels a row) blurred in place by about
 * a Gaussian of radius r. False only when there was no memory for it. */
static inline int wall_blur(uint32_t *px, int w, int h, int stride, int r) {
	if (r <= 0 || w <= 0 || h <= 0) return 1;
	/* each box a third of the radius: three of them reach about r */
	int box = (r + 1) / 3 > 0 ? (r + 1) / 3 : 1;
	int longest = w > h ? w : h;
	uint32_t *line = malloc((size_t)longest * sizeof(*line) * 2);
	if (!line) return 0;
	uint32_t *a = line, *b = line + longest;
	for (int y = 0; y < h; y++) {
		uint32_t *row = px + (size_t)y * stride;
		memcpy(a, row, (size_t)w * sizeof(*a));
		wall_box_run(b, a, w, 1, box);
		wall_box_run(a, b, w, 1, box);
		wall_box_run(row, a, w, 1, box);
	}
	for (int x = 0; x < w; x++) {
		for (int y = 0; y < h; y++) a[y] = px[(size_t)y * stride + x];
		wall_box_run(b, a, h, 1, box);
		wall_box_run(a, b, h, 1, box);
		wall_box_run(b, a, h, 1, box);
		for (int y = 0; y < h; y++) px[(size_t)y * stride + x] = b[y];
	}
	free(line);
	return 1;
}

#endif
