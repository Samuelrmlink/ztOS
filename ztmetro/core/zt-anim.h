/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Animation speeds (ROADMAP 8.66): one speed per kind of motion, and one over
 * all of them, each a step from 0.25x to 3x or Off. The shell, every Metro app
 * and Settings read them through this header, so a duration is never taken as
 * it is written in the code: zt_anim_ms() turns it into what the user chose.
 *
 * In the config (~/.config/ztmetro/config), per cent of normal speed:
 *
 *   anim_global = 75          the master; off = no animation anywhere
 *   anim_app_open = 100       one per kind below (anim_ + its key); off = none
 *   slow_motion = on          the developer's 5x, over all of it
 *
 * Older files said `menu_speed = N` (the one "Animation speed" there was):
 * that is read as anim_global when the file has no anim_global, and Settings
 * keeps writing it beside anim_global for an older shell. ztmetro/ANIMATIONS.md
 * lists every animation and the kind it is under.
 *
 * Kept free of anything but libc so tests/anim-speed.c can run it on any machine.
 */
#ifndef ZT_ANIM_H
#define ZT_ANIM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum zt_anim_kind {
	ZT_ANIM_APP_OPEN,	/* Start's tiles turning away, the app swinging in, its rows' entrance */
	ZT_ANIM_APP_CLOSE,	/* the app swinging back, Start's tiles returning */
	ZT_ANIM_APP_PAGES,	/* a Metro app's pages turning (not Settings'), its pickers and boards */
	ZT_ANIM_SWITCHER,	/* the task switcher */
	ZT_ANIM_SETTINGS_LINK,	/* into a Settings page from a quick tile, search or another app */
	ZT_ANIM_SETTINGS_PAGES,	/* Settings' own pages turning */
	ZT_ANIM_START,		/* Start and the app list: swiping between, edit mode, a tile's menu, search */
	ZT_ANIM_PRESS,		/* the tilt under a finger */
	ZT_ANIM_SCROLL,		/* coasting after a flick, and the spring back from an end */
	ZT_ANIM_SHADE,		/* the action centre opening and closing, a notification swiped away */
	ZT_ANIM_QUICK_PANELS,	/* the brightness and torch panels a quick tile opens */
	ZT_ANIM_LIVE_TILES,	/* live tiles turning over */
	ZT_ANIM_LOCK,		/* the lock screen and its passcode pad, the boot logo */
	ZT_ANIM_VOLUME,		/* the volume panel */
	ZT_ANIM_TOAST,		/* a notification's toast */
	ZT_ANIM_DIALOG,		/* the shell's dialogs */
	ZT_ANIM_KEYBOARD,	/* the on-screen keyboard coming and going */
	ZT_ANIM_ROTATION,	/* the screen turning */
	ZT_ANIM_BARS,		/* the status bar: full-screen bars, icons, the screenshot flash */
	ZT_ANIM_N
};

enum zt_anim_group {
	ZT_ANIM_G_APPS, ZT_ANIM_G_SETTINGS, ZT_ANIM_G_START, ZT_ANIM_G_SHADE, ZT_ANIM_G_TILES,
	ZT_ANIM_G_SYSTEM, ZT_ANIM_G_N
};

static const char *const ZT_ANIM_GROUPS[ZT_ANIM_G_N] = {
	"App transitions", "Settings", "Start", "Shade & quick tiles", "Live tiles", "System",
};

struct zt_anim_info {
	const char *key;	/* anim_<key> in the config */
	const char *name;	/* Settings' row */
	const char *detail;	/* the line under it */
	int group;
	/* the master speed applies (its Off always does). The screen's turn and
	 * nothing else keeps its own time: it is matched to the panel's. */
	bool global;
};

static const struct zt_anim_info ZT_ANIM_INFO[ZT_ANIM_N] = {
	[ZT_ANIM_APP_OPEN] = { "app_open", "Opening an app", "Start's tiles turn away and the app swings in",
		ZT_ANIM_G_APPS, true },
	[ZT_ANIM_APP_CLOSE] = { "app_close", "Closing an app", "The app swings away and the tiles come back",
		ZT_ANIM_G_APPS, true },
	[ZT_ANIM_APP_PAGES] = { "app_pages", "Pages in apps", "Rows turning between an app's pages, its pickers",
		ZT_ANIM_G_APPS, true },
	[ZT_ANIM_SWITCHER] = { "switcher", "Task switcher", "Opening it, picking a card, swiping one away",
		ZT_ANIM_G_APPS, true },
	[ZT_ANIM_SETTINGS_LINK] = { "settings_link", "Opening a Settings page",
		"From a quick tile held, search or another app", ZT_ANIM_G_SETTINGS, true },
	[ZT_ANIM_SETTINGS_PAGES] = { "settings_pages", "Settings pages", "Rows turning between Settings' pages",
		ZT_ANIM_G_SETTINGS, true },
	[ZT_ANIM_START] = { "start", "Start and the app list", "Swiping between them, editing, a tile's menu, search",
		ZT_ANIM_G_START, true },
	[ZT_ANIM_PRESS] = { "press", "Press tilt", "Tiles and rows tilting under a finger",
		ZT_ANIM_G_START, true },
	[ZT_ANIM_SCROLL] = { "scroll", "Scrolling", "Coasting after a flick, the bounce at an end",
		ZT_ANIM_G_START, true },
	[ZT_ANIM_SHADE] = { "shade", "Action centre", "Opening and closing it, a notification swiped away",
		ZT_ANIM_G_SHADE, true },
	[ZT_ANIM_QUICK_PANELS] = { "quick_panels", "Quick tile panels", "Brightness and torch levels",
		ZT_ANIM_G_SHADE, true },
	[ZT_ANIM_LIVE_TILES] = { "live_tiles", "Live tile flips", "A tile turning over to its back and front",
		ZT_ANIM_G_TILES, true },
	[ZT_ANIM_LOCK] = { "lock", "Lock screen", "Unlocking, the passcode pad, the logo at start",
		ZT_ANIM_G_SYSTEM, true },
	[ZT_ANIM_VOLUME] = { "volume", "Volume panel", "Coming down and going", ZT_ANIM_G_SYSTEM, true },
	[ZT_ANIM_TOAST] = { "toast", "Notification toasts", "A toast swivelling in and out",
		ZT_ANIM_G_SYSTEM, true },
	[ZT_ANIM_DIALOG] = { "dialog", "Dialogs", "The shell's questions dropping in", ZT_ANIM_G_SYSTEM, true },
	[ZT_ANIM_KEYBOARD] = { "keyboard", "Keyboard", "Sliding up and down, the app moving above it",
		ZT_ANIM_G_SYSTEM, true },
	[ZT_ANIM_ROTATION] = { "rotation", "Screen rotation", "The turn itself (not slowed by the master speed)",
		ZT_ANIM_G_SYSTEM, false },
	[ZT_ANIM_BARS] = { "bars", "Status bar", "Bars over a full-screen app, icons, the screenshot flash",
		ZT_ANIM_G_SYSTEM, true },
};

/* The steps offered, per cent of normal speed; 0 is Off. */
static const int ZT_ANIM_STEPS[] = { 0, 25, 50, 75, 100, 125, 150, 200, 300 };
#define ZT_ANIM_N_STEPS ((int)(sizeof(ZT_ANIM_STEPS) / sizeof(ZT_ANIM_STEPS[0])))
static const char *const ZT_ANIM_STEP_NAMES[] = {
	"Off", "0.25x (slowest)", "0.5x", "0.75x", "1x", "1.25x", "1.5x", "2x", "3x (fastest)",
};

#define ZT_ANIM_GLOBAL_DEFAULT	75	/* the shell's speed since 2026-11 (menu_speed's default) */
#define ZT_ANIM_KIND_DEFAULT	100
#define ZT_ANIM_SLOW_FACTOR	5	/* slow_motion */

struct zt_anim_cfg {
	int global;		/* per cent; 0: off */
	int kind[ZT_ANIM_N];	/* per cent; 0: off */
	bool slow;		/* slow_motion: everything five times as long */
	bool global_set;	/* anim_global was read: an old menu_speed no longer counts */
};

static inline void zt_anim_defaults(struct zt_anim_cfg *c) {
	c->global = ZT_ANIM_GLOBAL_DEFAULT;
	for (int i = 0; i < ZT_ANIM_N; i++) c->kind[i] = ZT_ANIM_KIND_DEFAULT;
	c->slow = false;
	c->global_set = false;
}

/* The step nearest a per cent (Off only for 0 or less). */
static inline int zt_anim_snap(int percent) {
	if (percent <= 0) return 0;
	int best = 1;
	for (int i = 1; i < ZT_ANIM_N_STEPS; i++) {
		int d = abs(ZT_ANIM_STEPS[i] - percent), bd = abs(ZT_ANIM_STEPS[best] - percent);
		if (d < bd) best = i;
	}
	return ZT_ANIM_STEPS[best];
}

static inline int zt_anim_step_index(int percent) {
	int p = zt_anim_snap(percent);
	for (int i = 0; i < ZT_ANIM_N_STEPS; i++) {
		if (ZT_ANIM_STEPS[i] == p) return i;
	}
	return 0;
}

/* A value as written: "off", "on" (normal), "150" (per cent), "1.5x". */
static inline int zt_anim_parse_value(const char *v) {
	while (*v == ' ' || *v == '\t') v++;
	if (!strcmp(v, "off") || !strcmp(v, "none")) return 0;
	if (!strcmp(v, "on") || !strcmp(v, "normal")) return 100;
	char *end;
	double d = strtod(v, &end);
	if (end == v) return -1;	/* not a value: ignored */
	while (*end == ' ') end++;
	if (*end == 'x' || *end == 'X') d *= 100.0;
	return zt_anim_snap((int)(d + 0.5));
}

static inline int zt_anim_kind_named(const char *key) {
	for (int i = 0; i < ZT_ANIM_N; i++) {
		if (!strcmp(key, ZT_ANIM_INFO[i].key)) return i;
	}
	return -1;
}

/* One line of the config, its key and value trimmed. True if it was ours. */
static inline bool zt_anim_config_line(struct zt_anim_cfg *c, const char *key, const char *value) {
	if (!strcmp(key, "slow_motion")) {
		c->slow = !strcmp(value, "on") || !strcmp(value, "1");
		return true;
	}
	if (!strcmp(key, "menu_speed")) {
		/* the old "Animation speed": the master, unless the file has one */
		int v = zt_anim_parse_value(value);
		if (!c->global_set && v > 0) c->global = v;
		return true;
	}
	if (!strcmp(key, "anim_global")) {
		int v = zt_anim_parse_value(value);
		if (v >= 0) {
			c->global = v;
			c->global_set = true;
		}
		return true;
	}
	if (strncmp(key, "anim_", 5) != 0) return false;
	int k = zt_anim_kind_named(key + 5);
	if (k < 0) return false;	/* someone else's anim_ key */
	int v = zt_anim_parse_value(value);
	if (v >= 0) c->kind[k] = v;
	return true;
}

/* The speed a kind runs at, per cent (0: off). */
static inline int zt_anim_speed(const struct zt_anim_cfg *c, int kind) {
	if (kind < 0 || kind >= ZT_ANIM_N) kind = ZT_ANIM_APP_PAGES;
	if (c->global <= 0 || c->kind[kind] <= 0) return 0;
	return ZT_ANIM_INFO[kind].global ? c->global * c->kind[kind] / 100 : c->kind[kind];
}

static inline bool zt_anim_off(const struct zt_anim_cfg *c, int kind) {
	return zt_anim_speed(c, kind) <= 0;
}

/* How long `ms` of a kind's motion lasts as chosen: 0 when it is off, else at
 * least 1 ms for anything that had a length. Slow motion on top. */
static inline uint32_t zt_anim_ms(const struct zt_anim_cfg *c, int kind, uint32_t ms) {
	int speed = zt_anim_speed(c, kind);
	if (speed <= 0 || ms == 0) return 0;
	uint64_t out = ((uint64_t)ms * 100 + (uint64_t)speed / 2) / (uint64_t)speed;
	if (c->slow) out *= ZT_ANIM_SLOW_FACTOR;
	if (out == 0) out = 1;
	return out > 0x7fffffffu ? 0x7fffffffu : (uint32_t)out;
}

/* Something waiting for a motion to end (a timer): never 0, which disarms a
 * wl_event_source timer; an animation that is off is waited for 1 ms. */
static inline uint32_t zt_anim_wait_ms(const struct zt_anim_cfg *c, int kind, uint32_t ms) {
	uint32_t w = zt_anim_ms(c, kind, ms);
	return w ? w : 1;
}

/* How much faster than written the motion's clock runs (2.0: twice as fast;
 * slow motion counted); 0 when it is off. For motion stepped by elapsed time. */
static inline double zt_anim_rate(const struct zt_anim_cfg *c, int kind) {
	int speed = zt_anim_speed(c, kind);
	if (speed <= 0) return 0;
	return speed / 100.0 / (c->slow ? ZT_ANIM_SLOW_FACTOR : 1);
}

/* The row Settings shows for a value. */
static inline const char *zt_anim_step_name(int percent) {
	return ZT_ANIM_STEP_NAMES[zt_anim_step_index(percent)];
}

#endif
