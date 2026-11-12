// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The lock screen's "what keeps it awake" (ROADMAP 8.30): a small line over
 * the clock, "Awake: Brave playing · 2 more", when something other than the
 * lock screen itself keeps the phone from sleeping, and a read-only sheet
 * with every blocker and the last hour's wakers when it is tapped.
 *
 * The code is lock-awake.c, included at the end of ztmetro.c so it can use
 * the shell's own helpers; these are the calls ztmetro.c makes into it. The
 * data comes from zrn-sleep-why (zrn-sleepd's reader), run as a child only
 * when the lock screen lights, every LOCK_AWAKE_REFRESH_MS while it stays
 * lit and locked, and when the sheet opens: never with the screen off.
 *
 * Config (~/.config/ztmetro/config, Settings > lock screen): lock_awake =
 * on (the default: only when something else keeps it awake), always (also
 * "the lock screen only"), off.
 */
#ifndef ZT_LOCK_AWAKE_H
#define ZT_LOCK_AWAKE_H

#include <stdbool.h>

struct zt_server;

/* load_config(): the lock_awake key (NULL: the default) */
static void lock_awake_config(const char *value);
/* lock(): the lock screen is up; with the screen lit, ask zrn-sleep-why */
static void lock_awake_refresh(struct zt_server *server);
/* screen_set(): the screen went off: no timer, no sheet */
static void lock_awake_screen_off(struct zt_server *server);
/* the unlock: the sheet goes, and what it was asked to open after the unlock opens */
static void lock_awake_unlocked(struct zt_server *server);
/* touch_begin() on the lock screen: what is under the finger (-1: nothing of ours) */
static int lock_awake_hit(struct zt_server *server, double x, double y);
/* touch_end(): a tap that went down on `index`; true if it was ours */
static bool lock_awake_tap(struct zt_server *server, int index, double x, double y);
/* the frame: the line and the sheet over the lock screen at lock_y, the
 * clock block raised by `lift` (0..1) */
static void lock_awake_draw(struct zt_server *server, float lift);
/* --render: the line and the sheet with a sample reply, to PNG files */
static void lock_awake_render_sample(struct zt_server *server);

#endif
