// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztDeskMode: desktop mode's controls as an app of their own, from its tile:
 * whether XFCE runs and where, the monitor and its mode, Start and Stop,
 * starting by itself when a monitor connects, the virtual screen. The page
 * is desktop-page.c, the same one Settings has under display; it talks to
 * the shell through $XDG_RUNTIME_DIR/ztmetro.ctl and reads its state from
 * ztmetro-desktop.state.
 */
#define _GNU_SOURCE
#include <string.h>

#include "zrnmetrocore.h"
#include "desktop-page.h"

static double render(cairo_t *cr, int page) {
	(void)page;
	return desktop_page_render(cr);
}

static void activate(int action, int arg) {
	if (action >= DESKTOP_ACTIONS && action < DESKTOP_ACTIONS_END) desktop_page_activate(action, arg);
}

static void arrived(int page) {
	(void)page;
	desktop_page_arrived();
}

/* the shell's state changes by itself (a monitor comes, XFCE ends): look again */
static void tick(void) {
	desktop_page_poll();	/* read at the next tick, or the next redraw */
	zmc_redraw();
}

static const struct zmc_app app = {
	.app_id = "ztdeskmode", .title = "Desktop mode", .caps = "DESKTOP MODE",
	.render = render, .activate = activate, .arrived = arrived,
};

int main(int argc, char **argv) {
	desktop_page_arrived();
	if (argc == 3 && !strcmp(argv[1], "--render")) {
		return zmc_render_png(&app, 0, 900, argv[2]);
	}
	zmc_every(2000, tick);
	return zmc_run(&app, 0);
}
