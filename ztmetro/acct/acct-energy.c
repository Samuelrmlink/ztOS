// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztacctd: time in front. (The power model comes in its own commit.)
 */
#include "acctd.h"

static double front_since;

/* the time since the last change goes to what was in front then */
void energy_screen_change(void) {
	if (front_since > 0 && now.screen_on) {
		struct tally d = { .fg_s = now.boot_s - front_since };
		group_add(now.front_g, &d);
	}
	front_since = now.boot_s;
}

void energy_start(void) {}
void energy_tick(double dt_boot, double dt_mono) {
	(void)dt_boot;
	(void)dt_mono;
	energy_screen_change();
}
void energy_settle(bool force) { (void)force; }
void energy_status(FILE *f) { fprintf(f, "\"energy\":{\"model\":\"none yet\"}"); }
void energy_add_net(int g, struct proc *p, int cls, double bytes, double packets) {
	(void)g; (void)p; (void)cls; (void)bytes; (void)packets;
}
void energy_save_calib(void) {}
