/* make check-pivot-wheel: fingers on a pivot through the core's real touch handling
 * and its real frames, with Wayland and GL as stubs: both ways, round both ends, a
 * change of mind, a quick run of swipes, a short drag, an app's own territory; that
 * the page that opens is the one that was shown on the way; and that every title in
 * the strip is drawn whole. Nothing to show it on. */
#define _GNU_SOURCE
#include "../core/zrnmetrocore.c"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include "swipe-cost/fake-platform.h"

int wl_display_dispatch_pending(struct wl_display *a) { return 0; }

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); puts(""); } } while (0)

#define PLOT_TOP 400	/* tab 0 has a territory of its own from here, like the equalizer's plot */
static const char *const TITLES[] = { "status", "listening", "controls", "about" };
static int n_tabs = 4, renders, arrivals, plot_touches;
static bool with_plot;

static double render(cairo_t *cr, int page) {
	renders++;
	struct zmc_tab tabs[4];
	for (int i = 0; i < n_tabs; i++) tabs[i] = (struct zmc_tab){ TITLES[i], i };
	double y = zmc_pivot_wheel(cr, tabs, n_tabs, page);
	/* each tab's own colour, to tell its picture by */
	cairo_set_source_rgb(cr, (page + 1) * 40 / 255.0, 0, 0);
	cairo_rectangle(cr, 0, 200, zmc.width, 100);
	cairo_fill(cr);
	return y + 900;
}
static void arrived(int page) { arrivals++; }
static bool touch(int phase, int id, double x, double y, uint32_t time) {
	if (phase == 0 && (!with_plot || zmc.page != 0 || y < PLOT_TOP)) return false;
	plot_touches++;
	return true;
}
static const struct zmc_app app = { .app_id = "t", .caps = "T", .render = render, .arrived = arrived, .touch = touch };

static uint32_t t_ms = 1000;
static void down(double x, double y) {
	touch_down(NULL, NULL, 0, t_ms += 16, NULL, 0, wl_fixed_from_double(x), wl_fixed_from_double(y));
}
static void to(double x, double y) {
	touch_motion(NULL, NULL, t_ms += 16, 0, wl_fixed_from_double(x), wl_fixed_from_double(y));
}
static void up(void) { touch_up(NULL, NULL, 0, t_ms += 16, 0); }

static void frame(void) {
	if (zmc.frame) frame_done(NULL, zmc.frame, 0);
	else if (zmc.dirty || zmc.cache_stale) present();
}
static void settle(void) {
	for (int i = 0; i < 2000 && (zmc.slide_active || zmc.frame || zmc.dirty || zmc.cache_stale); i++) {
		usleep(1000);
		frame();
	}
	CHECK(!zmc.slide_active, "still sliding");
}
/* the app left alone: what the core does when idle */
static void idle(void) {
	settle();
	for (int i = 0; i < 8 && pivot_wants_prepare(); i++) pivot_prepare();
}
/* a stroke from x0 to x1, over `steps` points of 16 ms */
static void stroke(double x0, double x1, int steps) {
	down(x0, 120);
	for (int i = 1; i <= steps; i++) to(x0 + (x1 - x0) * i / steps, 120);
	frame();
}
static void flick_left(void) { stroke(320, 40, 8); up(); }
static void flick_right(void) { stroke(40, 320, 8); up(); }

/* the colour on the screen at the middle of tab k's picture, as the next frame would
 * draw it: which page that picture is of, or -1 */
static int page_in(cairo_surface_t *s) {
	if (!s) return -1;
	cairo_surface_flush(s);
	const unsigned char *d = cairo_image_surface_get_data(s);
	int st = cairo_image_surface_get_stride(s);
	return d[250 * zmc.scale * st + 10 * 4 + 2] / 40 - 1;	/* (red, of BGRA) */
}
static int shown_going(int dir) {
	return page_in(dir > 0 || zmc.wheel.n == 2 ? zmc.piv_next : zmc.piv_prev);
}

static void start(int tabs, int page) {
	n_tabs = tabs;
	zmc.page = page;
	zmc.scroll = 0;
	pivot_drop_all();
	zmc.cache_stale = true;
	zmc.anim_active = false;
	present();
	idle();
}

/* The strip: every title in sight has all its ink, wherever the tabs have got to.
 * Drawn the plain way, for the pixels. */
static void check_titles_at(double dx) {
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, zmc.width, zmc.height);
	cairo_t *cr = cairo_create(s);
	draw_pivot(cr, 1);
	cairo_destroy(cr);
	cairo_surface_flush(s);
	const unsigned char *d = cairo_image_surface_get_data(s);
	int st = cairo_image_surface_get_stride(s);
	/* each title in sight, one by one: its ink reaches both ends of what is on screen */
	double u = -dx / zmc.width, f = u - floor(u);
	long k0 = (long)floor(u);
	double strip = wheel_pos(k0) + f * (wheel_pos(k0 + 1) - wheel_pos(k0));
	for (long k = k0 - 6; k < k0 + 8; k++) {
		double x = MARGIN - 3 + wheel_pos(k) - strip, w = zmc.wheel.w[wheel_wrap(k)];
		int a = x < 0 ? 0 : (int)x, b = x + w > zmc.width - 1 ? zmc.width - 1 : (int)(x + w);
		if (b - a < 16) continue;	/* (a sliver of it: nothing to tell) */
		int first = -1, last = -1;
		for (int c = a; c <= b; c++) {
			for (int y = 40; y < 90; y++) {
				if (d[y * st + c * 4 + 1] > 30) {
					if (first < 0) first = c;
					last = c;
				}
			}
		}
		CHECK(first >= 0 && first <= a + 12 && last >= b - 12, "dx %.0f: %s at %.0f: on screen %d..%d, ink %d..%d", dx,
			zmc.wheel.tabs[wheel_wrap(k)].title, x, a, b, first, last);
	}
	cairo_surface_destroy(s);
}
/* Anything drawn in the title strip between two columns, the tabs dragged by dx? */
static bool ink_between(double x0, double x1, double dx) {
	zmc.swipe_dx = dx;
	bool was = zmc.swiping;
	zmc.swiping = true;
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, zmc.width, zmc.height);
	cairo_t *cr = cairo_create(s);
	draw_pivot(cr, 1);
	cairo_destroy(cr);
	cairo_surface_flush(s);
	const unsigned char *d = cairo_image_surface_get_data(s);
	int st = cairo_image_surface_get_stride(s);
	bool any = false;
	for (int x = (int)x0 < 0 ? 0 : (int)x0; x < x1 && x < zmc.width; x++) {
		for (int y = 40; y < 90; y++) {
			any = any || d[y * st + x * 4 + 1] > 30;
		}
	}
	cairo_surface_destroy(s);
	zmc.swiping = was;
	if (!was) zmc.swipe_dx = 0;
	return any;
}

static void check_titles(double dx) {
	zmc.swipe_dx = dx;
	zmc.swiping = true;
	check_titles_at(dx);
	zmc.swiping = false;
	zmc.swipe_dx = 0;
}
static void check_titles_held(void) { check_titles_at(zmc.swipe_dx); }

int main(void) {
	zmc.app = &app;
	zmc.width = 360;
	zmc.height = 664;
	zmc.scale = 1;
	zmc.accent = 0xff0000;
	zmc.configured = true;
	zmg_init(zmc.display, zmc.surface, zmc.width, zmc.height, zmc.scale);

	/* ---- four tabs: right to left, and round */
	start(4, 0);
	for (int i = 1; i <= 8; i++) {
		int want = i % 4;
		CHECK(shown_going(1) == want, "forward %d: the picture ready is of %d, %d comes next", i, shown_going(1), want);
		flick_left();
		idle();
		CHECK(zmc.page == want, "forward %d: landed on %d, wanted %d", i, zmc.page, want);
		CHECK(zmc.wheel.tabs[zmc.wheel.at].page == zmc.page, "forward %d: the wheel says another", i);
	}
	/* ---- left to right, and round the other way */
	start(4, 0);
	for (int i = 1; i <= 8; i++) {
		int want = (8 - i) % 4;
		CHECK(shown_going(-1) == want, "backward %d: the picture ready is of %d, %d comes next", i, shown_going(-1), want);
		flick_right();
		idle();
		CHECK(zmc.page == want, "backward %d: landed on %d, wanted %d", i, zmc.page, want);
	}
	/* ---- there and back: status, listening, status */
	start(4, 0);
	flick_left();
	idle();
	CHECK(zmc.page == 1, "there: %d", zmc.page);
	flick_right();
	idle();
	CHECK(zmc.page == 0, "and back: %d", zmc.page);

	/* ---- what is shown under the finger is what opens, with no idle time at all */
	start(4, 0);
	for (int i = 0; i < 6; i++) {
		int from = zmc.page;
		int dir = i % 3 == 2 ? -1 : 1, want = (from + dir + 4) % 4;
		stroke(dir > 0 ? 320 : 40, dir > 0 ? 40 : 320, 8);
		frame();
		CHECK(shown_going(dir) == want, "unprepared %d: shown %d, wanted %d", i, shown_going(dir), want);
		up();
		settle();
		CHECK(zmc.page == want, "unprepared %d: landed on %d, wanted %d", i, zmc.page, want);
	}

	/* ---- a change of mind in the same drag: the way it ends is the way it goes */
	start(4, 0);
	stroke(100, 50, 5);	/* towards listening */
	for (int i = 1; i <= 14; i++) to(50 + 20 * i, 120);	/* and on, well past where it began */
	frame();
	CHECK(shown_going(-1) == 3, "change of mind: shown %d", shown_going(-1));
	to(340, 120);
	t_ms += 200;	/* stopped before lifting */
	up();
	idle();
	CHECK(zmc.page == 3, "change of mind: landed on %d", zmc.page);

	/* ---- a short drag settles back, either way */
	start(4, 2);
	stroke(200, 170, 6);
	t_ms += 200;
	up();
	idle();
	CHECK(zmc.page == 2 && zmc.swipe_dx == 0, "short left: %d, at %.0f", zmc.page, zmc.swipe_dx);
	stroke(200, 230, 6);
	t_ms += 200;
	up();
	idle();
	CHECK(zmc.page == 2 && zmc.swipe_dx == 0, "short right: %d, at %.0f", zmc.page, zmc.swipe_dx);

	/* ---- a quick run of swipes, each catching the last in flight: one tab each */
	start(4, 0);
	int expect = 0;
	for (int i = 0; i < 10; i++) {
		flick_left();
		expect = (expect + 1) % 4;
		for (int f = 0; f < 3; f++) { usleep(16000); frame(); }	/* a few frames, and the finger is back */
	}
	idle();
	CHECK(zmc.page == expect, "quick run: landed on %d, wanted %d", zmc.page, expect);

	/* ---- one long drag takes the tabs on past the next one */
	start(4, 0);
	down(350, 120);
	to(330, 120);
	to(300, 120);
	for (int i = 1; i <= 14; i++) to(350 - 50 * i, 120);	/* (off the screen, past two widths) */
	frame();
	t_ms += 200;
	up();
	idle();
	CHECK(zmc.page == 2 || zmc.page == 1, "long drag: landed on %d", zmc.page);

	/* ---- a drag begun at once after a swipe, and held: all the titles still there */
	start(4, 0);
	for (int i = 0; i < 5; i++) {
		flick_left();
		settle();	/* landed, and nothing drawn ahead */
		int page = zmc.page;
		stroke(300, 150, 6);
		check_titles_held();
		frame();
		CHECK(shown_going(1) == (page + 1) % 4, "held %d: shown %d", i, shown_going(1));
		t_ms += 200;
		zmc.swipe_dx = -150;
		up();
		settle();
		CHECK(zmc.page == page, "held %d: 150 of 360 and let go: landed on %d, not %d", i, zmc.page, page);
	}
	/* and going the other way, its title comes in from the left: the tab before */
	start(4, 1);
	stroke(100, 250, 6);
	check_titles_held();
	frame();
	CHECK(shown_going(-1) == 0, "backward, held: shown %d", shown_going(-1));
	up();
	idle();

	/* ---- a burst: six swipes, a quarter second apart, from controls: status */
	start(4, 2);
	for (int i = 0; i < 6; i++) {
		flick_left();
		for (int f = 0; f < 16; f++) { usleep(16000); frame(); }
	}
	idle();
	CHECK(zmc.page == (2 + 6) % 4, "burst: landed on %d, wanted %d", zmc.page, (2 + 6) % 4);
	/* and back the other way, by as many */
	for (int i = 0; i < 6; i++) {
		flick_right();
		for (int f = 0; f < 16; f++) { usleep(16000); frame(); }
	}
	idle();
	CHECK(zmc.page == 2, "burst back: landed on %d", zmc.page);

	/* ---- what the log counts is this slide's: not carried over from one that settled back */
	start(4, 0);
	stroke(200, 170, 6);
	t_ms += 200;
	up();
	settle();
	stroke(320, 40, 8);
	up();
	CHECK(pivot_frames == 0 && pivot_worst == 0, "carried over: %d frames, worst %u", pivot_frames, pivot_worst);
	settle();

	/* ---- every title whole in the strip, both ways */
	start(4, 1);
	for (double dx = -350; dx <= 350; dx += 25) check_titles(dx);
	start(4, 3);
	for (double dx = -350; dx <= 350; dx += 50) check_titles(dx);

	/* ---- three tabs */
	start(3, 0);
	for (int i = 1; i <= 7; i++) {
		flick_left();
		idle();
		CHECK(zmc.page == i % 3, "three forward %d: %d", i, zmc.page);
	}
	start(3, 0);
	for (int i = 1; i <= 7; i++) {
		flick_right();
		idle();
		CHECK(zmc.page == (9 - i) % 3, "three backward %d: %d", i, zmc.page);
	}

	/* ---- two tabs: as ever, either way is the other one, and what a swipe draws */
	start(2, 0);
	for (int i = 1; i <= 6; i++) {
		int before = renders;
		i % 2 ? flick_left() : flick_right();
		idle();
		CHECK(zmc.page == i % 2, "two %d: %d", i, zmc.page);
		CHECK(renders - before <= 1, "two %d: %d pages drawn for a swipe", i, renders - before);
	}

	/* ---- loop off: the ends are ends */
	zmc.tabs_loop = false;
	start(4, 0);
	for (int i = 1; i <= 5; i++) {
		flick_left();
		idle();
		CHECK(zmc.page == (i < 3 ? i : 3), "stop forward %d: %d", i, zmc.page);
		CHECK(zmc.swipe_dx == 0, "stop forward %d: left at %.0f", i, zmc.swipe_dx);
	}
	/* the last tab: nothing after its title, at rest, and dragged on past the end */
	double end = MARGIN - 3 + zmc.wheel.w[3];
	CHECK(!ink_between(end + 8, zmc.width, 0), "a title past the last tab, at rest");
	for (int i = 0; i < zmc.n_hits; i++) {
		CHECK(zmc.hits[i].action != ZMC_ACT_PIVOT, "a next title to tap on the last tab");
	}
	stroke(340, 40, 8);	/* and a long drag: no further than its end */
	CHECK(zmc.page == 3, "long drag past the end: %d", zmc.page);
	t_ms += 200;
	up();
	idle();
	CHECK(zmc.page == 3 && zmc.swipe_dx == 0, "settled back past the end: %d at %.0f", zmc.page, zmc.swipe_dx);
	for (int i = 2; i >= -2; i--) {
		flick_right();
		idle();
		CHECK(zmc.page == (i > 0 ? i : 0), "stop backward %d: %d", i, zmc.page);
	}
	/* the first tab, dragged right: nothing to the left of its title */
	stroke(100, 200, 6);
	frame();
	CHECK(!ink_between(0, MARGIN - 3 + 100 * 0.1 - 2, 100), "a title before the first tab");
	up();
	idle();
	CHECK(zmc.page == 0, "settled back before the start: %d", zmc.page);
	/* two tabs with the ends on: each goes to the other one way only */
	start(2, 0);
	flick_right();
	idle();
	CHECK(zmc.page == 0, "two, stop: %d", zmc.page);
	flick_left();
	idle();
	CHECK(zmc.page == 1, "two, on: %d", zmc.page);
	flick_left();
	idle();
	CHECK(zmc.page == 1, "two, stop at the last: %d", zmc.page);
	flick_right();
	idle();
	CHECK(zmc.page == 0, "two, back: %d", zmc.page);
	zmc.tabs_loop = true;

	/* ---- what a page has as its own keeps its gesture */
	with_plot = true;
	start(4, 0);
	int touches = plot_touches;
	down(320, 500);
	to(200, 500);
	to(80, 500);
	frame();
	CHECK(!zmc.swiping && plot_touches > touches, "the plot lost its finger");
	up();
	idle();
	CHECK(zmc.page == 0, "the plot's drag changed the tab: %d", zmc.page);
	with_plot = false;

	/* ---- the page's pictures do not outlive a page turn */
	start(4, 0);
	zmc_go(2, 1);
	CHECK(!zmc.piv_cur && !zmc.piv_next && !zmc.piv_prev, "pictures kept across a page turn");
	zmc.anim_active = false;
	zmc.anim_target_pending = false;

	if (failures) {
		printf("FAIL: %d\n", failures);
		return 1;
	}
	puts("PASS: a pivot is a wheel: both ways, round, and what shows is what opens");
	return 0;
}
