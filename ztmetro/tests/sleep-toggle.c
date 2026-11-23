/* The quick menu's Sleep tile reading zrn-sleepd's files (make check-sleep-toggle). */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../sleep-toggle.h"

static char state[256], run[256];

static void put(const char *dir, const char *name, const char *text) {
	char path[300];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "w");
	assert(f);
	fputs(text, f);
	fclose(f);
}

static void drop(const char *dir, const char *name) {
	char path[300];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	unlink(path);
}

static struct sleep_tile look(void) {
	struct sleep_tile t;
	sleep_tile_read(&t, state, run);
	return t;
}

int main(void) {
	char base[] = "/tmp/sleep-toggle-XXXXXX";
	assert(mkdtemp(base));
	snprintf(state, sizeof(state), "%s/var", base);
	snprintf(run, sizeof(run), "%s/run", base);

	/* no zrn-sleepd at all: faint */
	struct sleep_tile t = look();
	assert(t.mode == SLEEP_MISSING && !sleep_tile_on(&t));
	assert(!strcmp(sleep_tile_note(&t), "Unavailable"));
	mkdir(state, 0755);
	mkdir(run, 0755);

	/* off by hand: no note */
	t = look();
	assert(t.mode == SLEEP_OFF && !sleep_tile_on(&t) && sleep_tile_note(&t) == NULL);

	/* on, then paused by the firmware (still on) */
	put(state, "enabled", "");
	t = look();
	assert(t.mode == SLEEP_ON && sleep_tile_on(&t) && sleep_tile_note(&t) == NULL);
	put(run, "paused", "until=1234\n");
	t = look();
	assert(t.mode == SLEEP_PAUSED && sleep_tile_on(&t) && !strcmp(sleep_tile_note(&t), "Paused"));
	drop(run, "paused");

	/* switched off by zrn-sleepd itself: distinct, and why */
	drop(state, "enabled");
	put(state, "off_reason", "nowake The phone did not wake from its last sleep.\n");
	t = look();
	assert(t.mode == SLEEP_OFF_ITSELF && !strcmp(t.kind, "nowake"));
	assert(!strcmp(sleep_tile_note(&t), "Didn't wake last time"));
	put(state, "off_reason", "driver Its driver did not load.\n");
	t = look();
	assert(!strcmp(sleep_tile_note(&t), "Driver didn't load"));
	put(state, "off_reason", "recovery Device recovery failed after sleep.\n");
	t = look();
	assert(!strcmp(sleep_tile_note(&t), "Didn't recover"));
	put(state, "off_reason", "somethingnew text\n");
	t = look();
	assert(!strcmp(sleep_tile_note(&t), "Switched itself off"));
	/* an empty reason file is just off */
	put(state, "off_reason", "");
	t = look();
	assert(t.mode == SLEEP_OFF);

	/* a stale reason does not count while it is on */
	put(state, "off_reason", "nowake x\n");
	put(state, "enabled", "");
	t = look();
	assert(t.mode == SLEEP_ON);

	struct sleep_tile a = { SLEEP_OFF_ITSELF, "nowake" }, b = { SLEEP_OFF_ITSELF, "driver" };
	assert(!sleep_tile_same(&a, &b) && sleep_tile_same(&a, &a));

	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", base);
	assert(system(cmd) == 0);
	puts("sleep-toggle: ok");
	return 0;
}
