/* Keeping still pictures from burning into the OLED panel (ROADMAP 8.37).
 *
 * Two parts, outside the always-on display (which has its own, power/aod):
 *
 *  - The shift. What stands still for hours while the screen is on (the
 *    status bar's icons and clock, the nav bar's three buttons, the lock
 *    screen's clock) is drawn a few panel pixels off its place, the place
 *    changing every BURNIN_STEP_S round BURNIN_PLACES places. Each place
 *    is next to the one before (one step sideways, up or down, or both), so
 *    a move is a pixel or three, never a jump; each gets the same share of
 *    the time. The place is read from the wall clock, so a restarted shell
 *    carries on where the last one was. It costs no frame: the status bar
 *    and the lock clock are drawn again on the minute anyway and take the
 *    new place then; the nav bar, which is not, is drawn again in that same
 *    minute's frame when its place has changed (once every BURNIN_STEP_S).
 *
 *  - The heartbeat. A hung shell leaves its last picture lit until the
 *    battery ends. Once a minute (the clock tick, which runs anyway) the
 *    shell writes $XDG_RUNTIME_DIR/ztmetro.alive: its pid, the monotonic
 *    time, whether the panel should be lit, and how long a frame it wants
 *    has been waiting. zrn-sleepd reads it (ShellWatch) and ends a shell
 *    whose heartbeat stopped while the panel is lit.
 *
 * The schedule is plain C with no libraries (tests/burn-in.c); the drawing
 * glue below it is only compiled with cairo. */
#ifndef ZT_BURN_IN_H
#define ZT_BURN_IN_H

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BURNIN_STEP_S	180	/* each place this long: round all nine in 27 min */
#define BURNIN_PLACES	9

/* A 3 x 3 walk: the middle, then round it. Each place is next to the one
 * before it, and the last next to the first, so no move is more than one
 * step either way. */
static const signed char BURNIN_PATH[BURNIN_PLACES][2] = {
	{ 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 },
};

/* What moves, how far one step goes (panel pixels; a step's the most it is
 * ever off its place), and where it is on the walk at a given time: the bars
 * at different points of it, so the two never move the same way at once. */
struct burnin_elem {
	const char *name;
	int step_x, step_y;	/* panel pixels */
	int phase;		/* places ahead on the walk */
};

static const struct burnin_elem BURNIN_STATUS = { "status bar", 3, 2, 0 };
static const struct burnin_elem BURNIN_NAV = { "nav bar", 3, 2, 4 };
static const struct burnin_elem BURNIN_LOCK = { "lock clock", 4, 4, 2 };

/* The place at wall-clock time t (seconds). */
static inline int burnin_place(long long t) {
	if (t < 0) t = 0;
	return (int)((t / BURNIN_STEP_S) % BURNIN_PLACES);
}

/* An element's offset at a place, in panel pixels. */
static inline void burnin_offset(const struct burnin_elem *e, int place, int *dx, int *dy) {
	int p = ((place % BURNIN_PLACES) + BURNIN_PLACES + e->phase) % BURNIN_PLACES;
	*dx = BURNIN_PATH[p][0] * e->step_x;
	*dy = BURNIN_PATH[p][1] * e->step_y;
}

/* The shell's own: switched on (config burn_in_shift, on by default) and
 * the place being drawn (-1: none yet). place_nav is the place the nav bar
 * was last drawn at. */
struct burnin_state {
	bool on;
	int place, place_nav;
	double commit_s;	/* CLOCK_MONOTONIC of the last frame shown */
};
static struct burnin_state g_burnin = { .on = true, .place = 0, .place_nav = -1 };

static inline double burnin_mono(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* A frame reached the panel. */
static inline void burnin_frame_shown(void) {
	g_burnin.commit_s = burnin_mono();
}

/* The place now, or 0 (the true place) with the shift off. */
static inline int burnin_place_now(long long t) {
	return g_burnin.on ? burnin_place(t) : 0;
}

/* The heartbeat file (see the top). CLOCK_MONOTONIC on both sides: it stops
 * while the SoC sleeps, so a phone woken after hours asleep does not read as
 * a shell silent for hours. `lit`: the panel should be showing something
 * (the screen on, or the always-on display); `waiting`: seconds a frame the
 * shell asked for has gone without reaching the panel (0: none waiting).
 * Written whole to a new file and renamed over the old, so a reader never
 * sees half of one. Returns 0, or -errno. */
static int burnin_heartbeat_write(const char *path, long pid, double mono, bool lit, double waiting) {
	char tmp[600], text[160];
	if (snprintf(tmp, sizeof(tmp), "%s.new", path) >= (int)sizeof(tmp)) return -ENAMETOOLONG;
	int n = snprintf(text, sizeof(text), "pid %ld\nmono %.0f\nlit %d\nwaiting %.0f\nstep %d\n",
		pid, mono, lit ? 1 : 0, waiting > 0 ? waiting : 0, BURNIN_STEP_S);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
	if (fd < 0) return -errno;
	bool ok = write(fd, text, (size_t)n) == n;
	int err = errno;
	close(fd);
	if (!ok) {
		unlink(tmp);
		return -(err ? err : EIO);
	}
	if (rename(tmp, path) != 0) {
		err = errno;
		unlink(tmp);
		return -err;
	}
	return 0;
}

#ifdef CAIRO_H
/* Move what `cr` draws next by the element's offset at the current place.
 * Points are panel pixels over `scale`, so the step is whole pixels at any
 * display scaling and in either orientation (it is the drawing that moves,
 * before the output's transform). */
static void burnin_translate(cairo_t *cr, const struct burnin_elem *e, double scale) {
	if (!g_burnin.on || scale <= 0) return;
	int dx, dy;
	burnin_offset(e, g_burnin.place, &dx, &dy);
	if (dx || dy) cairo_translate(cr, dx / scale, dy / scale);
}
#endif

#endif
