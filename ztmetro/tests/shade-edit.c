/* The action centre's quick-action editor: its list logic and the `quick`
 * key it shares with Settings (make check-shade-edit). */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define ZT_SHADE_EDIT_LOGIC_ONLY
#include "../shade-edit.h"

/* the shell's ids, in its order (ztmetro.c QUICK[]), and Settings' (QUICK_IDS) */
static const char *const IDS[] = { "wifi", "data", "bluetooth", "airplane", "rotation", "flashlight",
	"location", "brightness", "settings", "vibrate", "audio", "sleep", "none" };
enum { WIFI, DATA, BT, AIR, ROT, TORCH, LOC, BRIGHT, SET, VIB, AUDIO, SLEEP, NONE, N };

/* the defaults, as the shell and Settings start (the rest empty) */
#define DEFAULTS { WIFI, DATA, BT, AIR, AUDIO, TORCH, BRIGHT, SET, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE }

static void same(const int s[], const int want[], int n) {
	for (int i = 0; i < n; i++) assert(s[i] == want[i]);
}

static void test_parse_format(void) {
	int s[SE_MAX] = DEFAULTS, n = 0;
	char out[400];
	/* the owner's list (8.49, two rows) round-trips, and stays two rows */
	assert(se_parse("wifi,data,bluetooth,airplane,audio,flashlight,brightness,settings", IDS, N, NONE, s, &n) == 8);
	assert(n == 8);
	se_format(s, n, IDS, out, sizeof(out));
	assert(!strcmp(out, "wifi,data,bluetooth,airplane,audio,flashlight,brightness,settings"));
	/* spaces, unknown words, empty places, and a short list keeping the rest */
	int t[SE_MAX] = DEFAULTS;
	assert(se_parse(" sleep, none ,bogus,location", IDS, N, NONE, t, &n) == 3);
	int want[SE_MAX] = { SLEEP, NONE, LOC, AIR, AUDIO, TORCH, BRIGHT, SET };
	assert(n == 8);
	same(t, want, 8);
	se_format(t, n, IDS, out, sizeof(out));
	assert(!strcmp(out, "sleep,none,location,airplane,audio,flashlight,brightness,settings"));
	/* nine: a third row, its other places empty (not the defaults) */
	assert(se_parse("wifi,data,bluetooth,airplane,rotation,flashlight,location,brightness,settings", IDS, N, NONE, t, &n) == 9);
	assert(n == 12 && t[7] == BRIGHT && t[8] == SET && t[9] == NONE && t[11] == NONE);
	se_format(t, n, IDS, out, sizeof(out));
	assert(!strcmp(out, "wifi,data,bluetooth,airplane,rotation,flashlight,location,brightness,settings,none,none,none"));
	/* every tile and empty places: four rows, never more */
	assert(se_parse("wifi,data,bluetooth,airplane,rotation,flashlight,location,brightness,"
		"settings,vibrate,audio,sleep,none,none,none,none,wifi,data", IDS, N, NONE, t, &n) == 16);
	assert(n == 16 && t[11] == SLEEP && t[15] == NONE);
	se_format(t, n, IDS, out, sizeof(out));
	int u[SE_MAX] = DEFAULTS, m = 0;
	assert(se_parse(out, IDS, N, NONE, u, &m) == 16 && m == 16);
	same(u, t, 16);
	assert(se_parse(NULL, IDS, N, NONE, t, &n) == 0 && n == 8);
	/* a short buffer is cut, never overrun */
	char tiny[12];
	se_format(want, 8, IDS, tiny, sizeof(tiny));
	assert(strlen(tiny) < sizeof(tiny));
	/* the longest list there can be fits the shell's and Settings' buffers */
	int longest[SE_MAX];
	for (int i = 0; i < SE_MAX; i++) longest[i] = BRIGHT;	/* (the longest id) */
	se_format(longest, SE_MAX, IDS, out, sizeof(out));
	assert(strlen(out) + strlen("quick = \n") < 240);
}

static void test_rows(void) {
	assert(se_rows_of(0) == 8 && se_rows_of(8) == 8 && se_rows_of(9) == 12 && se_rows_of(12) == 12);
	assert(se_rows_of(13) == 16 && se_rows_of(40) == 16);
	int s[SE_MAX] = DEFAULTS;
	/* growing to a place in the row under the last: one row more, empty */
	assert(se_grow(s, 8, 5, NONE) == 8);
	int n = se_grow(s, 8, 9, NONE);
	assert(n == 12 && s[8] == NONE && s[11] == NONE);
	assert(se_grow(s, n, 16, NONE) == 12);	/* past four rows: no */
	s[9] = SLEEP;
	/* trimming: a row with a tile stays; empty ones at the end go */
	assert(se_trim(s, 12, NONE) == 12);
	s[9] = NONE;
	assert(se_trim(s, 12, NONE) == 8);
	/* two rows stay even when empty */
	int e[SE_MAX];
	for (int i = 0; i < SE_MAX; i++) e[i] = NONE;
	assert(se_trim(e, 16, NONE) == 8);
	/* an empty row between is kept: only those at the end go */
	e[12] = WIFI;
	assert(se_trim(e, 16, NONE) == 16);
}

static void test_move(void) {
	int s[SE_MAX] = DEFAULTS;
	se_move(s, 8, 0, 3);		/* forward: the ones between move back */
	int a[] = { DATA, BT, AIR, WIFI, AUDIO, TORCH, BRIGHT, SET };
	same(s, a, 8);
	se_move(s, 8, 7, 1);		/* back: the ones between move on */
	int b[] = { DATA, SET, BT, AIR, WIFI, AUDIO, TORCH, BRIGHT };
	same(s, b, 8);
	se_move(s, 8, 2, 2);		/* onto itself: nothing */
	se_move(s, 8, -1, 2);
	se_move(s, 8, 2, 8);		/* past the places: nothing */
	same(s, b, 8);
	/* across the rows (place 3 is the first row's last, 4 the second's first) */
	se_move(s, 8, 3, 4);
	int c[] = { DATA, SET, BT, WIFI, AIR, AUDIO, TORCH, BRIGHT };
	same(s, c, 8);
}

/* A tile dragged into the row the editor shows under the last: the shell's
 * shade_edit_up does se_grow, se_drop, then se_trim when it saves. */
static void test_third_row(void) {
	int s[SE_MAX] = DEFAULTS, n = 8;
	n = se_grow(s, n, 9, NONE);
	se_drop(s, n, 2, 9, NONE);		/* Bluetooth to row 3, place 2 */
	n = se_trim(s, n, NONE);
	int want[] = { WIFI, DATA, NONE, AIR, AUDIO, TORCH, BRIGHT, SET, NONE, BT, NONE, NONE };
	assert(n == 12);
	same(s, want, 12);
	char out[400];
	se_format(s, n, IDS, out, sizeof(out));
	assert(!strcmp(out, "wifi,data,none,airplane,audio,flashlight,brightness,settings,none,bluetooth,none,none"));
	/* read back by the other side (Settings, or the shell after a restart) */
	int t[SE_MAX] = DEFAULTS, m = 0;
	se_parse(out, IDS, N, NONE, t, &m);
	assert(m == 12);
	same(t, want, 12);
	/* onto a tile in the new row: the ones between move along, as before */
	se_drop(s, n, 0, 9, NONE);
	int want2[] = { DATA, NONE, AIR, AUDIO, TORCH, BRIGHT, SET, NONE, BT, WIFI, NONE, NONE };
	same(s, want2, 12);
	/* back up out of it: the row empties and goes */
	se_drop(s, n, 8, 1, NONE);
	se_drop(s, n, 9, 7, NONE);
	n = se_trim(s, n, NONE);
	assert(n == 8 && s[1] == BT && s[7] == WIFI);
	/* a fourth row, from a tray tile */
	n = se_grow(s, n, 9, NONE);
	se_place(s, n, 9, SLEEP, NONE);
	n = se_grow(s, n, 13, NONE);
	assert(n == 16);
	se_place(s, n, 13, LOC, NONE);
	assert(se_trim(s, n, NONE) == 16);
	/* the tray leaves out what the extra rows hold */
	int tray[N], k = se_tray(s, n, N, NONE, tray);
	for (int i = 0; i < k; i++) assert(tray[i] != SLEEP && tray[i] != LOC);
}

static void test_remove_place_add(void) {
	int s[SE_MAX] = DEFAULTS, n = 8;
	se_remove(s, n, 3, NONE);
	assert(s[3] == NONE && se_first_empty(s, n, NONE) == 3);
	/* the tray: what is not placed, in the shell's order, without `none` */
	int tray[N], k = se_tray(s, n, N, NONE, tray);
	int want_tray[] = { AIR, ROT, LOC, VIB, SLEEP };
	assert(k == 5);
	for (int i = 0; i < k; i++) assert(tray[i] == want_tray[i]);
	/* a tap on a tray tile: the first empty place */
	assert(se_add(s, n, SLEEP, NONE));
	assert(s[3] == SLEEP);
	assert(!se_add(s, n, LOC, NONE));	/* no empty place left in two rows */
	assert(!se_add(s, n, WIFI, NONE));	/* already there */
	/* the shell then grows a row for it (shade_edit_up) */
	n = se_grow(s, n, n, NONE);
	assert(n == 12 && se_add(s, n, LOC, NONE) && s[8] == LOC);
	/* dropped on a place: what was there goes back to the tray */
	se_place(s, n, 0, VIB, NONE);
	assert(s[0] == VIB && se_find(s, n, WIFI) < 0);
	k = se_tray(s, n, N, NONE, tray);
	assert(k == 3 && tray[0] == WIFI);
	/* a tile already placed, dropped on another place, moves (never twice) */
	se_place(s, n, 7, VIB, NONE);
	assert(s[7] == VIB && s[0] == NONE);
	se_place(s, n, 2, NONE, NONE);		/* `none` is never carried */
	assert(s[2] == BT);
	se_place(s, n, 12, WIFI, NONE);		/* past the places: nothing */
	assert(se_find(s, SE_MAX, WIFI) < 0);
}

static void test_geometry(void) {
	/* portrait: four across; the tray below the hint, as the places */
	double x, y;
	se_tray_rect(5, 4, 4, 200, 86, 57, 3, &x, &y);
	assert(x == 4 + 1 * 89 && y == 200 + 1 * 60);
	double rects[SE_MAX][4];
	for (int i = 0; i < SE_MAX; i++) {
		rects[i][0] = 4 + (i % 4) * 89;
		rects[i][1] = 50 + (i / 4) * 60;
		rects[i][2] = 86;
		rects[i][3] = 57;
	}
	assert(se_slot_at(rects, 8, 4 + 43, 50 + 28, 6) == 0);
	assert(se_slot_at(rects, 8, 4 + 89 * 3 + 43, 50 + 60 + 28, 6) == 7);
	/* in the gap between two: the nearer centre */
	assert(se_slot_at(rects, 8, 4 + 86 + 1, 50 + 28, 6) == 0);
	assert(se_slot_at(rects, 8, 4 + 86 + 2.5, 50 + 28, 6) == 1);
	/* the third row: a target only while the editor shows it */
	assert(se_slot_at(rects, 8, 4 + 89 + 43, 50 + 120 + 28, 6) == -1);
	assert(se_slot_at(rects, 12, 4 + 89 + 43, 50 + 120 + 28, 6) == 9);
	assert(se_slot_at(rects, 16, 4 + 89 * 3 + 43, 50 + 180 + 28, 6) == 15);
	/* far below (over the tray): none */
	assert(se_slot_at(rects, 16, 100, 400, 6) == -1);
	/* turned: eight across, so twelve places are a row and a half */
	for (int i = 0; i < SE_MAX; i++) {
		rects[i][0] = 4 + (i % 8) * 92;
		rects[i][1] = 50 + (i / 8) * 62;
		rects[i][2] = 89;
		rects[i][3] = 59;
	}
	assert(se_slot_at(rects, 8, 4 + 92 * 6 + 40, 80, 6) == 6);
	assert(se_slot_at(rects, 12, 4 + 92 * 2 + 40, 50 + 62 + 30, 6) == 10);
	assert(se_slot_at(rects, 12, 4 + 92 * 6 + 40, 50 + 62 + 30, 6) == -1);
}

int main(void) {
	test_parse_format();
	test_rows();
	test_move();
	test_third_row();
	test_remove_place_add();
	test_geometry();
	puts("shade-edit: ok");
	return 0;
}
