// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ZrnDevTest as an app of its own (`zrndevtest`, from a shell). Settings has the same
 * pages under developer: devtest-pages.c.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include "zrnmetrocore.h"
#include "devtest-pages.h"

static bool back(void) {
	devtest_leave();
	int up = devtest_parent(zmc.page, -1);
	if (up < 0) return false;	/* either tab of the hub: back leaves the app */
	zmc_go(up, -1);
	return true;
}

static const struct zmc_app devtest = {
	.app_id = "zrndevtest", .title = "ZrnDevTest", .caps = "ZRNDEVTEST",
	.render = devtest_render, .activate = devtest_activate, .back = back, .arrived = devtest_arrived,
	.overlay = devtest_overlay,
};

int main(int argc, char **argv) {
	static const char *names[] = { "hub", "system", "sensors", "journal", "kernel", "shell", "hardware", "logs", "animations", "preview", "temperatures", "charging" };
	if (argc == 4 && strcmp(argv[1], "--render") == 0) {
		/* ZT_ANIM=N ZT_ANIM_MS=T: the animations page with N played T ms in */
		devtest_pages_setup(0, getenv("ZT_ANIM") ? atoi(getenv("ZT_ANIM")) : -1, 0);
		for (int i = 0; i < 12; i++) {
			if (strcmp(names[i], argv[2]) == 0) {
				return zmc_render_png(&devtest, i, i == 8 ? 1400 : 664, argv[3]);
			}
		}
		return 2;
	}
	devtest_pages_setup(0, -1, 0);
	int status = zmc_run(&devtest, 0);
	devtest_leave();
	return status;
}
