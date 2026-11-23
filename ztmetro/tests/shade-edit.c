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

static void same(const int s[SE_SLOTS], const int want[SE_SLOTS]) {
	for (int i = 0; i < SE_SLOTS; i++) assert(s[i] == want[i]);
}

static void test_parse_format(void) {
	int s[SE_SLOTS] = { WIFI, DATA, BT, AIR, AUDIO, TORCH, BRIGHT, SET };	/* the defaults */
	char out[200];
	/* the owner's list round-trips */
	assert(se_parse("wifi,data,bluetooth,airplane,audio,flashlight,brightness,settings", IDS, N, s) == 8);
	se_format(s, IDS, out, sizeof(out));
	assert(!strcmp(out, "wifi,data,bluetooth,airplane,audio,flashlight,brightness,settings"));
	/* spaces, unknown words, empty places, and a short list keeping the rest */
	int t[SE_SLOTS] = { WIFI, DATA, BT, AIR, AUDIO, TORCH, BRIGHT, SET };
	assert(se_parse(" sleep, none ,bogus,location", IDS, N, t) == 3);
	int want[SE_SLOTS] = { SLEEP, NONE, LOC, AIR, AUDIO, TORCH, BRIGHT, SET };
	same(t, want);
	se_format(t, IDS, out, sizeof(out));
	assert(!strcmp(out, "sleep,none,location,airplane,audio,flashlight,brightness,settings"));
	/* more than eight: the first eight */
	assert(se_parse("wifi,data,bluetooth,airplane,rotation,flashlight,location,brightness,settings", IDS, N, t) == 8);
	assert(t[7] == BRIGHT);
	assert(se_parse(NULL, IDS, N, t) == 0);
	/* a short buffer is cut, never overrun */
	char tiny[12];
	se_format(want, IDS, tiny, sizeof(tiny));
	assert(strlen(tiny) < sizeof(tiny));
}

static void test_move(void) {
	int s[SE_SLOTS] = { WIFI, DATA, BT, AIR, AUDIO, TORCH, BRIGHT, SET };
	se_move(s, 0, 3);		/* forward: the ones between move back */
	int a[SE_SLOTS] = { DATA, BT, AIR, WIFI, AUDIO, TORCH, BRIGHT, SET };
	same(s, a);
	se_move(s, 7, 1);		/* back: the ones between move on */
	int b[SE_SLOTS] = { DATA, SET, BT, AIR, WIFI, AUDIO, TORCH, BRIGHT };
	same(s, b);
	se_move(s, 2, 2);		/* onto itself: nothing */
	se_move(s, -1, 2);
	se_move(s, 2, 8);
	same(s, b);
	/* across the rows (place 3 is the first row's last, 4 the second's first) */
	se_move(s, 3, 4);
	int c[SE_SLOTS] = { DATA, SET, BT, WIFI, AIR, AUDIO, TORCH, BRIGHT };
	same(s, c);
}

static void test_remove_place_add(void) {
	int s[SE_SLOTS] = { WIFI, DATA, BT, AIR, AUDIO, TORCH, BRIGHT, SET };
	se_remove(s, 3, NONE);
	assert(s[3] == NONE && se_first_empty(s, NONE) == 3);
	/* the tray: what is not placed, in the shell's order, without `none` */
	int tray[N], n = se_tray(s, N, NONE, tray);
	int want_tray[] = { AIR, ROT, LOC, VIB, SLEEP };
	assert(n == 5);
	for (int k = 0; k < n; k++) assert(tray[k] == want_tray[k]);
	/* a tap on a tray tile: the first empty place */
	assert(se_add(s, SLEEP, NONE));
	assert(s[3] == SLEEP);
	assert(!se_add(s, LOC, NONE));		/* no empty place left */
	assert(!se_add(s, WIFI, NONE));		/* already there */
	/* dropped on a place: what was there goes back to the tray */
	se_place(s, 0, LOC, NONE);
	assert(s[0] == LOC && se_find(s, WIFI) < 0);
	n = se_tray(s, N, NONE, tray);
	assert(n == 4 && tray[0] == WIFI);
	/* a tile already placed, dropped on another place, moves (never twice) */
	se_place(s, 7, LOC, NONE);
	assert(s[7] == LOC && s[0] == NONE);
	se_place(s, 2, NONE, NONE);		/* `none` is never carried */
	assert(s[2] == BT);
}

static void test_geometry(void) {
	/* portrait: four across; the tray below the hint, as the places */
	double x, y;
	se_tray_rect(5, 4, 4, 200, 86, 57, 3, &x, &y);
	assert(x == 4 + 1 * 89 && y == 200 + 1 * 60);
	double rects[SE_SLOTS][4];
	for (int i = 0; i < SE_SLOTS; i++) {
		rects[i][0] = 4 + (i % 4) * 89;
		rects[i][1] = 50 + (i / 4) * 60;
		rects[i][2] = 86;
		rects[i][3] = 57;
	}
	assert(se_slot_at(rects, 4 + 43, 50 + 28, 6) == 0);
	assert(se_slot_at(rects, 4 + 89 * 3 + 43, 50 + 60 + 28, 6) == 7);
	/* in the gap between two: the nearer centre */
	assert(se_slot_at(rects, 4 + 86 + 1, 50 + 28, 6) == 0);
	assert(se_slot_at(rects, 4 + 86 + 2.5, 50 + 28, 6) == 1);
	/* far below (over the tray): none */
	assert(se_slot_at(rects, 100, 400, 6) == -1);
	/* turned: eight across in one row */
	for (int i = 0; i < SE_SLOTS; i++) {
		rects[i][0] = 4 + i * 92;
		rects[i][1] = 50;
		rects[i][2] = 89;
		rects[i][3] = 59;
	}
	assert(se_slot_at(rects, 4 + 92 * 6 + 40, 80, 6) == 6);
}

int main(void) {
	test_parse_format();
	test_move();
	test_remove_place_add();
	test_geometry();
	puts("shade-edit: ok");
	return 0;
}
