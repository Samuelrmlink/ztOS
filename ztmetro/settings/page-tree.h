/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Settings' pages, and the page each one goes back up to. Kept out of
 * ztsettings.c so that it can be tested on any machine (tests/settings-tree.c).
 */
#ifndef ZTSETTINGS_PAGE_TREE_H
#define ZTSETTINGS_PAGE_TREE_H

enum page {
	PAGE_MAIN,
	PAGE_THEME,	/* customization */
	PAGE_DISPLAY,
	PAGE_WIFI,
	PAGE_MOBILE,
	PAGE_AUDIO,
	PAGE_BATTERY,
	PAGE_CPU,
	PAGE_VM,
	PAGE_GPU,
	PAGE_KEYBOARD,
	PAGE_ABOUT,
	PAGE_PERF,
	PAGE_DEV,
	PAGE_INSTALLED_APPS,
	PAGE_APP_DETAIL,
	PAGE_HAPTICS,
	PAGE_QUICK,
	PAGE_APN,
	PAGE_WIFI_JOIN,	/* a network's password, the first time */
	PAGE_WIFI_NET,	/* one network: its state, its address, forgetting it */
	PAGE_DATE,
	PAGE_BLUETOOTH,
	PAGE_ACCENTS,	/* the accent picker, full screen */
	PAGE_WALLS,	/* the background photo picker: categories, then one */
	PAGE_NAVIGATION,
	PAGE_BOOST,	/* Dynamic Boost System: RapidBoost and IntelligentBoost */
	PAGE_CONNECTIONS,	/* Wi-Fi, Bluetooth, the mobile network */
	PAGE_SOUND_TOUCH,	/* ringtones + sounds, haptics */
	PAGE_POWER,	/* battery + performance: power, then hardware */
	PAGE_SYSTEM,	/* date + region, keyboard, developer, about */
	PAGE_ANIMATION,	/* animation speeds */
	PAGE_STYLE,	/* the shell's style: chosen, then applied with a warning */
	PAGE_TILES,	/* Tiles Shell: the start screen's columns and background */
	PAGE_LOCK,	/* Lock Screen: its photo, and whether waking shows it */
	PAGE_APP_SCALE,	/* app scaling: a size for each app */
	PAGE_LED,	/* the front LED: what lights it, and in what colour */
	N_PAGES
};

#define PAGE_NO_PARENT	(-1)	/* the top of the tree: back leaves the app */

static const int PAGE_PARENTS[N_PAGES] = {
	[PAGE_MAIN] = PAGE_NO_PARENT,
	[PAGE_THEME] = PAGE_MAIN,
	[PAGE_DISPLAY] = PAGE_MAIN,
	[PAGE_CONNECTIONS] = PAGE_MAIN,
	[PAGE_WIFI] = PAGE_CONNECTIONS,
	[PAGE_MOBILE] = PAGE_CONNECTIONS,
	[PAGE_SOUND_TOUCH] = PAGE_MAIN,
	[PAGE_AUDIO] = PAGE_SOUND_TOUCH,
	[PAGE_POWER] = PAGE_MAIN,
	[PAGE_BATTERY] = PAGE_POWER,
	[PAGE_CPU] = PAGE_POWER,
	[PAGE_VM] = PAGE_POWER,
	[PAGE_GPU] = PAGE_POWER,
	[PAGE_SYSTEM] = PAGE_MAIN,
	[PAGE_KEYBOARD] = PAGE_SYSTEM,
	[PAGE_ABOUT] = PAGE_SYSTEM,
	[PAGE_PERF] = PAGE_POWER,
	[PAGE_DEV] = PAGE_SYSTEM,
	[PAGE_INSTALLED_APPS] = PAGE_MAIN,
	[PAGE_APP_DETAIL] = PAGE_INSTALLED_APPS,
	[PAGE_APP_SCALE] = PAGE_INSTALLED_APPS,
	[PAGE_HAPTICS] = PAGE_SOUND_TOUCH,
	[PAGE_LED] = PAGE_SOUND_TOUCH,
	[PAGE_QUICK] = PAGE_THEME,
	[PAGE_APN] = PAGE_MOBILE,
	[PAGE_WIFI_JOIN] = PAGE_WIFI,
	[PAGE_WIFI_NET] = PAGE_WIFI,
	[PAGE_DATE] = PAGE_SYSTEM,
	[PAGE_BLUETOOTH] = PAGE_CONNECTIONS,
	[PAGE_ACCENTS] = PAGE_THEME,
	[PAGE_TILES] = PAGE_THEME,
	[PAGE_LOCK] = PAGE_THEME,
	[PAGE_WALLS] = PAGE_TILES,	/* or Lock Screen: see page_parent */
	[PAGE_ANIMATION] = PAGE_THEME,
	[PAGE_STYLE] = PAGE_THEME,
	[PAGE_NAVIGATION] = PAGE_THEME,
	[PAGE_BOOST] = PAGE_POWER,
};

/* Pages by name: `ztsettings NAME` (Hyperscan opens one), --render NAME FILE,
 * and --list-pages for a script that wants them all. */
static const struct { const char *name; enum page page; } PAGE_NAMES[] = {
	{ "main", PAGE_MAIN },
	{ "installed-apps", PAGE_INSTALLED_APPS },
	{ "apps", PAGE_INSTALLED_APPS },	/* an old name: there was a tab called that */
	{ "app", PAGE_APP_DETAIL },
	{ "app-scale", PAGE_APP_SCALE },
	{ "customization", PAGE_THEME },
	{ "theme", PAGE_THEME },	/* its old name */
	{ "accents", PAGE_ACCENTS },
	{ "wallpapers", PAGE_WALLS },
	{ "animation", PAGE_ANIMATION },
	{ "style", PAGE_STYLE },
	{ "tiles", PAGE_TILES },
	{ "lock", PAGE_LOCK },
	{ "navigation", PAGE_NAVIGATION },
	{ "quick", PAGE_QUICK },
	{ "display", PAGE_DISPLAY },
	{ "connections", PAGE_CONNECTIONS },
	{ "wifi", PAGE_WIFI },
	{ "wifinet", PAGE_WIFI_NET },
	{ "wifijoin", PAGE_WIFI_JOIN },
	{ "bluetooth", PAGE_BLUETOOTH },
	{ "mobile", PAGE_MOBILE },
	{ "apn", PAGE_APN },
	{ "sound", PAGE_SOUND_TOUCH },
	{ "audio", PAGE_AUDIO },
	{ "haptics", PAGE_HAPTICS },
	{ "led", PAGE_LED },
	{ "power", PAGE_POWER },
	{ "battery", PAGE_BATTERY },
	{ "perf", PAGE_PERF },
	{ "boost", PAGE_BOOST },
	{ "cpu", PAGE_CPU },
	{ "gpu", PAGE_GPU },
	{ "vm", PAGE_VM },
	{ "system", PAGE_SYSTEM },
	{ "date", PAGE_DATE },
	{ "keyboard", PAGE_KEYBOARD },
	{ "dev", PAGE_DEV },
	{ "about", PAGE_ABOUT },
};
#define N_PAGE_NAMES (int)(sizeof(PAGE_NAMES) / sizeof(PAGE_NAMES[0]))

/* Whose photo the picker is choosing, and so where it goes back to. */
enum { WALL_TARGET_TILES = 1, WALL_TARGET_LOCK = 2 };

/* Where back goes from a page. */
static inline int page_parent(int page, int wall_target) {
	if (page == PAGE_WALLS && wall_target == WALL_TARGET_LOCK) return PAGE_LOCK;
	return page >= 0 && page < N_PAGES ? PAGE_PARENTS[page] : PAGE_NO_PARENT;
}

#endif
