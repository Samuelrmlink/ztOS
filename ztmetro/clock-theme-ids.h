/*
 * clock-theme-ids.h: the clock themes' ids and names, for Settings' pickers.
 * The themes themselves are the shell's (clock-themes.h, CLOCK_THEMES): keep
 * the two lists in the same order with the same ids.
 */
#ifndef CLOCK_THEME_IDS_H
#define CLOCK_THEME_IDS_H

#include <string.h>

enum { CLOCK_THEME_AOD = 1, CLOCK_THEME_LOCK = 2 };

static const struct { const char *id, *name; unsigned where; } CLOCK_THEME_IDS[] = {
	{ "metro", "Metro", CLOCK_THEME_AOD | CLOCK_THEME_LOCK },
	{ "compact", "Compact", CLOCK_THEME_AOD },
	{ "digital", "Digital", CLOCK_THEME_AOD },
	{ "analogue", "Analogue", CLOCK_THEME_AOD },
	{ "stacked", "Stacked", CLOCK_THEME_AOD },
	{ "calendar", "Calendar", CLOCK_THEME_AOD },
	{ "flip", "Flip clock", CLOCK_THEME_AOD | CLOCK_THEME_LOCK },
	{ "image", "Image", CLOCK_THEME_AOD },
};
#define N_CLOCK_THEME_IDS	(int)(sizeof(CLOCK_THEME_IDS) / sizeof(CLOCK_THEME_IDS[0]))

static inline const char *clock_theme_name(const char *id) {
	for (int i = 0; i < N_CLOCK_THEME_IDS; i++) {
		if (strcmp(CLOCK_THEME_IDS[i].id, id) == 0) return CLOCK_THEME_IDS[i].name;
	}
	return CLOCK_THEME_IDS[0].name;
}

#endif
