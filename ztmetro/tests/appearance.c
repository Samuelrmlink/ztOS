/* make check-appearance: the appearance keys (appearance.h) as the file and the
 * control pipe give them, and the config's migration away from DotMatrix. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../appearance.h"
#include "../settings/config-merge.h"

static char result[8192];

static const char *merge(const char *text, struct config_key *keys, int n) {
	FILE *in = text ? fmemopen((void *)text, strlen(text), "r") : NULL;
	FILE *out = fmemopen(result, sizeof(result), "w");
	assert(out);
	config_merge(in, out, keys, n);
	if (in) fclose(in);
	fclose(out);
	return result;
}

static bool is_look(const char *key) {
	return look_index(key) >= 0;
}

int main(void) {
	struct zt_look l;
	look_defaults(&l);

	/* the defaults are the look from before: opaque tiles, sharp, full
	 * brightness; no photo behind them; opaque switcher and search */
	assert(l.v[LOOK_TILE_OPACITY] == 100 && l.v[LOOK_TILE_FROST] == 0 && l.v[LOOK_TILE_BRIGHTNESS] == 100);
	assert(l.v[LOOK_BG_SHOW] == 0);
	assert(l.v[LOOK_SWITCHER_OPACITY] == 100 && l.v[LOOK_SEARCH_OPACITY] == 100);
	assert(l.v[LOOK_SWITCHER_BRIGHTNESS] == 100 && l.v[LOOK_SEARCH_BRIGHTNESS] == 100);
	for (int k = 0; k < N_LOOK; k++) {
		assert(LOOK_DEFS[k].def >= LOOK_DEFS[k].min && LOOK_DEFS[k].def <= LOOK_DEFS[k].max);
		assert(look_index(LOOK_DEFS[k].key) == k);
	}

	/* the file: numbers, a percent sign, on/off; out of range clamped; a bad
	 * value leaves it as it was; unknown keys are not ours */
	assert(look_parse(&l, "tile_opacity", "70"));
	assert(l.v[LOOK_TILE_OPACITY] == 70);
	assert(look_parse(&l, "tile_frost", "35%"));
	assert(l.v[LOOK_TILE_FROST] == 35);
	assert(look_parse(&l, "tile_opacity", "0"));
	assert(l.v[LOOK_TILE_OPACITY] == 10);		/* tiles never vanish */
	assert(look_parse(&l, "tile_brightness", "250"));
	assert(l.v[LOOK_TILE_BRIGHTNESS] == 100);
	assert(look_parse(&l, "search_opacity", "-5"));
	assert(l.v[LOOK_SEARCH_OPACITY] == 20);		/* its text stays readable */
	assert(look_parse(&l, "background_show", "on"));
	assert(l.v[LOOK_BG_SHOW] == 1);
	assert(look_parse(&l, "background_show", "maybe"));
	assert(l.v[LOOK_BG_SHOW] == 1);
	assert(look_parse(&l, "background_show", "0"));
	assert(l.v[LOOK_BG_SHOW] == 0);
	assert(look_parse(&l, "switcher_frost", "lots"));
	assert(l.v[LOOK_SWITCHER_FROST] == 100);
	assert(!look_parse(&l, "tiles_opacity", "50"));
	assert(!look_parse(&l, "accent", "lime"));

	char text[16];
	look_format(&l, LOOK_TILE_FROST, text, sizeof(text));
	assert(!strcmp(text, "35"));
	look_format(&l, LOOK_BG_SHOW, text, sizeof(text));
	assert(!strcmp(text, "off"));

	/* the pipe: "look KEY VALUE" (after the word); the index when it changed */
	assert(look_line(&l, "switcher_opacity 40\n") == LOOK_SWITCHER_OPACITY);
	assert(l.v[LOOK_SWITCHER_OPACITY] == 40);
	assert(look_line(&l, "switcher_opacity 40") == -1);	/* the same: nothing to do */
	assert(look_line(&l, "background_blur 55") == LOOK_BG_BLUR);
	assert(look_line(&l, "background_show on") == LOOK_BG_SHOW);
	assert(look_line(&l, "nonsense 5") == -1);
	assert(look_line(&l, "tile_opacity") == -1);
	assert(look_line(&l, "tile_opacity x") == -1);
	assert(look_line(&l, "") == -1);

	/* the blur's radius: none at 0, at least 2 px once on, 24 at most */
	assert(look_blur_radius(0) == 0);
	assert(look_blur_radius(1) == 2);
	assert(look_blur_radius(100) == 24);
	for (int p = 1; p < 100; p++) assert(look_blur_radius(p) <= look_blur_radius(p + 1));

	/* a slider let go: only appearance keys changed, so no SIGHUP */
	struct config_key k[3];
	memset(k, 0, sizeof(k));
	k[0].key = "accent";
	snprintf(k[0].value, sizeof(k[0].value), "cobalt");
	snprintf(k[0].loaded, sizeof(k[0].loaded), "cobalt");
	k[1].key = "tile_opacity";
	snprintf(k[1].value, sizeof(k[1].value), "80");
	snprintf(k[1].loaded, sizeof(k[1].loaded), "100");
	k[2].key = "search_frost";
	snprintf(k[2].value, sizeof(k[2].value), "100");
	snprintf(k[2].loaded, sizeof(k[2].loaded), "100");
	assert(config_only_changed_if(k, 3, is_look));
	snprintf(k[0].value, sizeof(k[0].value), "lime");
	assert(!config_only_changed_if(k, 3, is_look));	/* the accent too: the shell reloads */
	snprintf(k[0].value, sizeof(k[0].value), "cobalt");
	snprintf(k[1].value, sizeof(k[1].value), "100");
	assert(!config_only_changed_if(k, 3, is_look));	/* nothing changed at all */

	/* the migration: DotMatrix's keys go the next time Settings writes the
	 * file; everything else stays as it was, hand edits included */
	assert(config_key_retired("style"));
	assert(config_key_retired("dotmatrix_palette"));
	assert(config_key_retired("dotmatrix_dock3"));
	assert(!config_key_retired("styles"));
	assert(!config_key_retired("theme"));
	assert(!config_key_retired("lock_theme"));
	snprintf(k[1].value, sizeof(k[1].value), "60");
	assert(!strcmp(merge(
		"# ztMETRO configuration\n"
		"accent = cobalt\n"
		"style = dotmatrix\n"
		"dotmatrix_palette = Amber\n"
		"  dotmatrix_amoled=on\n"
		"theme = dark\n"
		"tile_opacity = 100\n"
		"dotmatrix_dock1 = ztphone\n"
		"unknown_key = 1\n", k, 3),
		"# ztMETRO configuration\n"
		"accent = cobalt\n"
		"theme = dark\n"
		"tile_opacity = 60\n"
		"unknown_key = 1\n"));
	/* a file from before 8.70 with nothing of this: the new key at the end */
	assert(!strcmp(merge("accent = cobalt\nstyle = metro\n", k, 3), "accent = cobalt\ntile_opacity = 60\n"));
	/* no file yet */
	assert(!strcmp(merge(NULL, k, 3), "tile_opacity = 60\n"));

	puts("appearance: ok");
	return 0;
}
