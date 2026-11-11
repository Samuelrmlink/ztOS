// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * ztPods: AirPods' batteries, listening modes and settings, as an app of its own, from
 * its tile. `ztpods MAC` opens on that pair. The pages are pods-pages.c, which Settings
 * has too.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "zrnmetrocore.h"
#include "pods-pages.h"

static void open_bluetooth(void) {
	/* (Settings is one window: this turns it to that page if it is open) */
	if (system("ztsettings bluetooth >/dev/null 2>&1 &") != 0) {}
}

/* On the phone or on a desktop (XFCE), as the window moves: zmc has given it
 * the shell's frame, window sizes and the mouse; the pages say "click" for
 * "tap" there (zmc_on_desktop), and are drawn again. */
static void pods_environment(bool desktop) {
	fprintf(stderr, "ztpods: on the %s\n", desktop ? "desktop" : "phone");
}

static const struct zmc_app app = {
	.app_id = "ztpods", .title = "AirPods", .caps = "AIRPODS",
	.render = pods_render, .activate = pods_activate, .slide = pods_slide, .back = pods_back,
	.arrived = pods_arrived, .text = pods_text, .environment = pods_environment,
};

static const struct { const char *name; int page; } PAGES[] = {
	{ "status", PODS_STATUS }, { "listening", PODS_LISTEN }, { "controls", PODS_CONTROLS }, { "about", PODS_ABOUT },
};

int main(int argc, char **argv) {
	pods_pages_setup(0, open_bluetooth);
	if (argc == 4 && !strcmp(argv[1], "--render")) {
		pods_show(NULL);
		const char *when = getenv("ZTPODS_RENDER_NOW");
		pods_render_as_of(when ? atol(when) : 0);
		for (size_t i = 0; i < sizeof(PAGES) / sizeof(PAGES[0]); i++) {
			if (!strcmp(PAGES[i].name, argv[2])) return zmc_render_png(&app, PAGES[i].page, 1200, argv[3]);
		}
		return 2;
	}
	pods_show(argc == 2 ? argv[1] : NULL);
	return zmc_run(&app, PODS_STATUS);
}
