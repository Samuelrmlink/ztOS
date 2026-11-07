/* make check-slider-touch: fingers on a real page through the core's real touch
 * handling: a slider on a long page, with a row beside it. Nothing to show it on. */
#define _GNU_SOURCE
#include "../core/zrnmetrocore.c"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <assert.h>
#include "swipe-cost/fake-platform.h"

enum { A_SLIDER = 5, A_ROW = 6 };
static int value = 50, slides, lifts, last_slide = -1, tapped;

static double render(cairo_t *cr, int page) {
	double y = zmc_header(cr, "test");
	y = zmc_entry(cr, y, "a row", "", A_ROW, 0);
	zmc_slider(cr, 16, 300, 328, value, "Volume", A_SLIDER);
	return 3000;
}
static void slide(int action, int percent, bool done) {
	assert(action == A_SLIDER);
	slides++;
	last_slide = percent;
	if (done) lifts++;
}
static void activate(int action, int arg) { tapped = action; }
static const struct zmc_app app = { .app_id = "t", .caps = "T", .render = render, .slide = slide, .activate = activate };

static uint32_t clock_ms_now = 1000;
static void down(double x, double y) {
	touch_down(NULL, NULL, 0, clock_ms_now += 16, NULL, 0, wl_fixed_from_double(x), wl_fixed_from_double(y));
}
static void to(double x, double y) {
	touch_motion(NULL, NULL, clock_ms_now += 16, 0, wl_fixed_from_double(x), wl_fixed_from_double(y));
}
static void up(void) {
	touch_up(NULL, NULL, 0, clock_ms_now += 16, 0);
	zmc.fling = false;
	zmc.vel = 0;
}
static void reset(void) {
	slides = lifts = tapped = 0;
	last_slide = -1;
	zmc.scroll = 0;
	zmc.axis = 0;
}

int wl_display_dispatch_pending(struct wl_display *a) { return 0; }

int main(void) {
	zmc.app = &app;
	zmc.width = 360;
	zmc.height = 664;
	zmc.scale = 1;
	zmc.accent = 0xff0000;
	/* the page, once, for its hits and height (not configured: no frames) */
	cache_ensure(1);
	double y0 = 300 + 24;	/* the slider's track, on screen at scroll 0 */

	/* touching it moves nothing; lifting without moving is a tap, as the finger lifts */
	reset();
	down(100, y0);
	assert(slides == 0 && !zmc.sliding);
	assert(zmc.slider_pending);
	up();
	assert(slides == 1 && lifts == 1);
	assert(last_slide == zmc_slider_percent(100, 16, 328, 0));
	assert(tapped == 0);	/* never the slider's action as a tap */

	/* straight up from it: the page scrolls, and the slider never hears of it */
	reset();
	down(100, y0);
	to(100, y0 - 4);
	assert(zmc.scroll == 0 && slides == 0);	/* not decided yet */
	to(100, y0 - 12);
	assert(!zmc.slider_pending && !zmc.sliding);
	to(100, y0 - 60);
	assert(zmc.scroll == 48);	/* from where it was decided: no jump */
	to(160, y0 - 70);		/* it stays the page's, wherever it wanders */
	assert(!zmc.sliding && slides == 0);
	up();
	assert(slides == 0 && lifts == 0 && tapped == 0);

	/* sideways: the slider, and the page stays */
	reset();
	down(100, y0);
	to(104, y0);
	assert(slides == 0);
	to(114, y0 + 2);
	assert(zmc.sliding && slides == 1 && zmc.scroll == 0);
	to(200, y0 + 150);		/* it stays the slider's, wherever it wanders */
	assert(zmc.sliding && zmc.scroll == 0);
	assert(last_slide == zmc_slider_percent(200, 16, 328, zmc.slider_grab));
	up();
	assert(lifts == 1 && !zmc.sliding);

	/* taken at the thumb, the thumb does not jump to the finger */
	reset();
	double thumb = 16 + 328 * value / 100.0;
	down(thumb + 15, y0);
	to(thumb + 25, y0);
	assert(zmc.sliding);
	assert(last_slide == value);
	to(thumb + 25 + 32.8, y0);
	assert(last_slide == value + 10);
	up();

	/* a finger on a moving list only stops it, slider or no slider */
	reset();
	zmc.fling = true;
	down(100, y0);
	assert(!zmc.slider_pending && !zmc.sliding);
	up();
	assert(slides == 0);

	/* the finger taken away: nothing is left held */
	reset();
	down(100, y0);
	touch_cancel(NULL, NULL);
	assert(!zmc.slider_pending && !zmc.sliding && slides == 0);
	zmc.fling = false;

	/* a row beside it still answers */
	reset();
	down(100, 120);
	up();
	assert(tapped == A_ROW && slides == 0);
	puts("PASS: a finger on a slider: vertical scrolls, sideways slides, no jump on touch-down");
	return 0;
}
