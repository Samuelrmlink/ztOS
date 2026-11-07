// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztSound: the equalizer and the effects of ztOS as an app of their own, from its
 * tile or as `ztsound apps`. The pages are sound-pages.c, which Settings has too.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "zrnmetrocore.h"
#include "sound-pages.h"

static const struct zmc_app app = {
	.app_id = "ztsound", .title = "Sound", .caps = "SOUND",
	.render = sound_render, .activate = sound_activate, .slide = sound_slide, .back = sound_back,
	.arrived = sound_arrived, .text = sound_text, .overlay = sound_overlay, .touch = sound_touch,
};

static const struct { const char *name; int page; } PAGES[] = {
	{ "equalizer", SOUND_EQ }, { "effects", SOUND_FX }, { "output", SOUND_OUT }, { "apps", SOUND_APPS },
};

int main(int argc, char **argv) {
	if (argc == 4 && !strcmp(argv[1], "--render")) {
		for (size_t i = 0; i < sizeof(PAGES) / sizeof(PAGES[0]); i++) {
			if (!strcmp(PAGES[i].name, argv[2])) {
				sound_pages_setup(0, false, true, PAGES[i].page);
				return zmc_render_png(&app, PAGES[i].page, 664, argv[3]);
			}
		}
		return 2;
	}
	int first = SOUND_EQ;
	if (argc == 2) {
		for (size_t i = 0; i < sizeof(PAGES) / sizeof(PAGES[0]); i++) {
			if (!strcmp(PAGES[i].name, argv[1])) first = PAGES[i].page;
		}
	}
	sound_pages_setup(0, false, false, first);
	return zmc_run(&app, first);
}
