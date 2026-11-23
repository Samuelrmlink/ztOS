/* make check-wall-blur: Start's photo blurred for the frosted tiles (wall-blur.h). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../wall-blur.h"

static int ch(uint32_t p, int c) {
	return (int)((p >> (8 * c)) & 255);
}

int main(void) {
	enum { W = 40, H = 30, S = 44 };	/* a stride wider than the picture */
	static uint32_t px[S * H];

	/* a flat picture stays as it is, whatever the radius */
	for (int i = 0; i < S * H; i++) px[i] = 0xff336699u;
	assert(wall_blur(px, W, H, S, 9));
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) assert(px[y * S + x] == 0xff336699u);
	}

	/* radius 0: nothing touched */
	px[0] = 0x12345678u;
	assert(wall_blur(px, W, H, S, 0));
	assert(px[0] == 0x12345678u);

	/* a white dot on black spreads out, symmetrical, its total kept, and
	 * the padding past the picture's width is never written */
	for (int i = 0; i < S * H; i++) px[i] = (i % S) >= W ? 0xdeadbeefu : 0xff000000u;
	px[15 * S + 20] = 0xffffffffu;
	assert(wall_blur(px, W, H, S, 6));
	int sum = 0;
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			uint32_t p = px[y * S + x];
			assert(ch(p, 3) == 255);			/* still opaque */
			assert(ch(p, 0) == ch(p, 1) && ch(p, 1) == ch(p, 2));	/* still grey */
			sum += ch(p, 0);
		}
		for (int x = W; x < S; x++) assert(px[y * S + x] == 0xdeadbeefu);
	}
	assert(sum > 150 && sum < 360);		/* 255 spread, give or take the rounding */
	assert(ch(px[15 * S + 20], 0) < 60);	/* the dot is gone ... */
	assert(ch(px[15 * S + 20], 0) > 0);
	assert(ch(px[15 * S + 18], 0) == ch(px[15 * S + 22], 0));	/* ... evenly round it */
	assert(ch(px[13 * S + 20], 0) == ch(px[17 * S + 20], 0));
	assert(ch(px[15 * S + 21], 0) >= ch(px[15 * S + 23], 0));	/* falling off */
	assert(ch(px[2 * S + 2], 0) == 0);	/* and nowhere near the corner */

	/* an edge: left half white, right half black; the middle grey, the far
	 * ends untouched (the edges are held, not faded to black) */
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) px[y * S + x] = x < W / 2 ? 0xffffffffu : 0xff000000u;
	}
	assert(wall_blur(px, W, H, S, 9));
	assert(ch(px[10 * S + 0], 0) == 255);
	assert(ch(px[10 * S + W - 1], 0) == 0);
	int mid = ch(px[10 * S + W / 2], 0) + ch(px[10 * S + W / 2 - 1], 0);
	assert(mid > 200 && mid < 310);
	for (int x = 1; x < W; x++) assert(ch(px[10 * S + x], 0) <= ch(px[10 * S + x - 1], 0));

	/* a picture smaller than the radius */
	uint32_t tiny[3] = { 0xff000000u, 0xffffffffu, 0xff000000u };
	assert(wall_blur(tiny, 3, 1, 3, 20));
	assert(ch(tiny[0], 0) == ch(tiny[2], 0) && ch(tiny[1], 0) >= ch(tiny[0], 0));

	puts("wall-blur: ok");
	return 0;
}
