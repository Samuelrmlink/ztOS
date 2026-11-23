// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The quick menu's Sleep tile (ROADMAP 8.47): whether zrn-sleepd may put the
 * SoC to sleep, and why it is off when it switched itself off. Plain C, so
 * tests/sleep-toggle.c checks it on the PC.
 *
 * zrn-sleepd's files (devices/samsung-starlte/overlay/usr/local/sbin/zrn-sleepd):
 *	/var/lib/zrn-sleep/enabled	there: sleeping is on
 *	/var/lib/zrn-sleep/off_reason	"KIND text": it switched itself off
 *					(nowake, driver, recovery); gone once
 *					switched by hand
 *	/run/zrn-sleepd/paused		the firmware refused; it tries again later
 * The tile switches it with `sudo -n /usr/local/sbin/zrn-sleepd enable|disable`
 * (sudoers.d/zrn-sleep-toggle: those two commands only).
 */
#ifndef ZT_SLEEP_TOGGLE_H
#define ZT_SLEEP_TOGGLE_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SLEEP_STATE_DIR	"/var/lib/zrn-sleep"
#define SLEEP_RUN_DIR	"/run/zrn-sleepd"
#define SLEEP_CMD	"/usr/local/sbin/zrn-sleepd"

enum sleep_mode {
	SLEEP_MISSING,		/* no zrn-sleepd on this phone: the tile is faint */
	SLEEP_ON,
	SLEEP_PAUSED,		/* on, but the firmware refused; it tries again */
	SLEEP_OFF,		/* switched off by hand */
	SLEEP_OFF_ITSELF,	/* zrn-sleepd switched it off: why in `why` */
};

struct sleep_tile {
	enum sleep_mode mode;
	char kind[16];		/* off_reason's first word */
};

/* The tile's second line, or NULL for none. */
static inline const char *sleep_tile_note(const struct sleep_tile *t) {
	switch (t->mode) {
	case SLEEP_PAUSED:
		return "Paused";
	case SLEEP_OFF_ITSELF:
		if (!strcmp(t->kind, "nowake")) return "Didn't wake last time";
		if (!strcmp(t->kind, "driver")) return "Driver didn't load";
		if (!strcmp(t->kind, "recovery")) return "Didn't recover";
		return "Switched itself off";
	case SLEEP_MISSING:
		return "Unavailable";
	default:
		return NULL;
	}
}

/* Read the state from `state_dir` and `run_dir` (the paths above; tests
 * give their own). */
static inline void sleep_tile_read(struct sleep_tile *t, const char *state_dir, const char *run_dir) {
	char path[256];
	memset(t, 0, sizeof(*t));
	if (access(state_dir, F_OK) != 0) {
		t->mode = SLEEP_MISSING;
		return;
	}
	snprintf(path, sizeof(path), "%s/enabled", state_dir);
	if (access(path, F_OK) == 0) {
		snprintf(path, sizeof(path), "%s/paused", run_dir);
		t->mode = access(path, F_OK) == 0 ? SLEEP_PAUSED : SLEEP_ON;
		return;
	}
	t->mode = SLEEP_OFF;
	snprintf(path, sizeof(path), "%s/off_reason", state_dir);
	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[128] = "";
	if (fgets(line, sizeof(line), f) && sscanf(line, "%15s", t->kind) == 1) {
		t->mode = SLEEP_OFF_ITSELF;
	}
	fclose(f);
}

static inline bool sleep_tile_on(const struct sleep_tile *t) {
	return t->mode == SLEEP_ON || t->mode == SLEEP_PAUSED;
}

static inline bool sleep_tile_same(const struct sleep_tile *a, const struct sleep_tile *b) {
	return a->mode == b->mode && !strcmp(a->kind, b->kind);
}

#endif
