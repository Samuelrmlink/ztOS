/* burn-in.h's schedule and heartbeat, on the host (make check-burn-in). */
#include "../burn-in.h"

#include <stdlib.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static const struct burnin_elem *const ELEMS[] = { &BURNIN_STATUS, &BURNIN_NAV, &BURNIN_LOCK };
#define N_ELEMS (int)(sizeof(ELEMS) / sizeof(ELEMS[0]))

static void schedule(void) {
	/* the place holds for a step, then moves to the next, round all nine */
	CHECK(burnin_place(0) == 0, "t=0");
	CHECK(burnin_place(BURNIN_STEP_S - 1) == 0, "end of the first step");
	CHECK(burnin_place(BURNIN_STEP_S) == 1, "the second step");
	CHECK(burnin_place((long long)BURNIN_STEP_S * BURNIN_PLACES) == 0, "round again");
	CHECK(burnin_place(-5) == 0, "a clock before 1970");
	/* a real date: a cycle's worth of seconds gives every place the same share */
	long long start = 1794500000LL, cycle = (long long)BURNIN_STEP_S * BURNIN_PLACES;
	int share[BURNIN_PLACES] = { 0 };
	int changes = 0, last = burnin_place(start);
	for (long long t = start; t < start + cycle; t++) {
		int p = burnin_place(t);
		share[p]++;
		if (p != last) changes++;
		last = p;
	}
	for (int p = 0; p < BURNIN_PLACES; p++) {
		CHECK(share[p] == BURNIN_STEP_S, "place %d held %d s, not %d", p, share[p], BURNIN_STEP_S);
	}
	/* the cost side: the nav bar is drawn once per change, at most this often */
	CHECK(changes <= BURNIN_PLACES, "%d changes in a cycle", changes);
	CHECK(BURNIN_STEP_S >= 60 && BURNIN_STEP_S % 60 == 0,
		"the step is whole minutes, so it changes on a clock tick: %d", BURNIN_STEP_S);
	printf("schedule: a place every %d s, round %d in %lld min; %d nav redraws an hour at most\n",
		BURNIN_STEP_S, BURNIN_PLACES, cycle / 60, 3600 / BURNIN_STEP_S);
}

static void walk(void) {
	/* the walk itself: nine different places, each next to the one before,
	 * the last next to the first, and balanced about the true place */
	int sx = 0, sy = 0;
	for (int p = 0; p < BURNIN_PLACES; p++) {
		int q = (p + 1) % BURNIN_PLACES;
		int ddx = abs(BURNIN_PATH[q][0] - BURNIN_PATH[p][0]), ddy = abs(BURNIN_PATH[q][1] - BURNIN_PATH[p][1]);
		CHECK(ddx <= 1 && ddy <= 1 && ddx + ddy > 0, "place %d to %d is not one step", p, q);
		for (int r = 0; r < p; r++) {
			CHECK(BURNIN_PATH[r][0] != BURNIN_PATH[p][0] || BURNIN_PATH[r][1] != BURNIN_PATH[p][1],
				"places %d and %d are the same", r, p);
		}
		sx += BURNIN_PATH[p][0];
		sy += BURNIN_PATH[p][1];
	}
	CHECK(sx == 0 && sy == 0, "the walk leans (%d, %d)", sx, sy);
}

static void offsets(void) {
	for (int e = 0; e < N_ELEMS; e++) {
		const struct burnin_elem *el = ELEMS[e];
		int sx = 0, sy = 0, maxx = 0, maxy = 0;
		for (int p = 0; p < BURNIN_PLACES; p++) {
			int dx, dy, nx, ny;
			burnin_offset(el, p, &dx, &dy);
			burnin_offset(el, p + 1, &nx, &ny);
			sx += dx;
			sy += dy;
			if (abs(dx) > maxx) maxx = abs(dx);
			if (abs(dy) > maxy) maxy = abs(dy);
			/* a move is at most one step each way: a few pixels, never a jump */
			CHECK(abs(nx - dx) <= el->step_x && abs(ny - dy) <= el->step_y,
				"%s jumps from place %d to %d", el->name, p, p + 1);
		}
		CHECK(sx == 0 && sy == 0, "%s leans (%d, %d)", el->name, sx, sy);
		/* the brief: about 2 to 4 panel pixels; the panel is 1440 across */
		CHECK(maxx >= 2 && maxx <= 4 && maxy >= 2 && maxy <= 4, "%s moves %d x %d px", el->name, maxx, maxy);
		printf("%s: up to %d px sideways and %d px up or down\n", el->name, maxx, maxy);
		/* a negative or large place is still a place */
		int ax, ay, bx, by;
		burnin_offset(el, -1, &ax, &ay);
		burnin_offset(el, BURNIN_PLACES - 1, &bx, &by);
		CHECK(ax == bx && ay == by, "%s: place -1", el->name);
	}
	/* the two bars never move the same way at once */
	for (int p = 0; p < BURNIN_PLACES; p++) {
		int s0, s1, n0, n1, s2, s3, n2, n3;
		burnin_offset(&BURNIN_STATUS, p, &s0, &s1);
		burnin_offset(&BURNIN_STATUS, p + 1, &s2, &s3);
		burnin_offset(&BURNIN_NAV, p, &n0, &n1);
		burnin_offset(&BURNIN_NAV, p + 1, &n2, &n3);
		CHECK(s2 - s0 != n2 - n0 || s3 - s1 != n3 - n1, "the bars move together at place %d", p);
	}
}

static void switched_off(void) {
	g_burnin.on = false;
	CHECK(burnin_place_now(1794500000LL + 4 * BURNIN_STEP_S) == 0, "off: the true place");
	g_burnin.on = true;
	CHECK(burnin_place_now((long long)BURNIN_STEP_S * 4) == 4, "on: the place");
}

static void heartbeat(void) {
	char dir[] = "/tmp/burnin-test-XXXXXX", path[300], text[300] = "";
	CHECK(mkdtemp(dir) != NULL, "mkdtemp");
	snprintf(path, sizeof(path), "%s/ztmetro.alive", dir);
	CHECK(burnin_heartbeat_write(path, 1234, 5678.4, true, 0) == 0, "write");
	CHECK(burnin_heartbeat_write(path, 1234, 5738.6, false, 61) == 0, "write again");
	FILE *f = fopen(path, "r");
	size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
	if (f) fclose(f);
	text[n] = 0;
	char expect[120];
	snprintf(expect, sizeof(expect), "pid 1234\nmono 5739\nlit 0\nwaiting 61\nstep %d\n", BURNIN_STEP_S);
	CHECK(strcmp(text, expect) == 0, "heartbeat reads:\n%s", text);
	char tmp[320];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	CHECK(access(tmp, F_OK) != 0, "the .new file was left");
	CHECK(burnin_heartbeat_write("/nonexistent-dir/x", 1, 1, true, 0) < 0, "an error is reported");
	unlink(path);
	rmdir(dir);
}

int main(void) {
	schedule();
	walk();
	offsets();
	switched_off();
	heartbeat();
	if (failures) {
		printf("%d failed\n", failures);
		return 1;
	}
	printf("burn-in: all passed\n");
	return 0;
}
