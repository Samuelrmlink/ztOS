/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Settings' pages, and the page each one goes back up to. Kept out of
 * ztsettings.c so that it can be tested on any machine (tests/settings-tree.c).
 */
#ifndef ZTSETTINGS_PAGE_TREE_H
#define ZTSETTINGS_PAGE_TREE_H

enum page {
	PAGE_MAIN,
	PAGE_THEME,
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
	PAGE_APPS,	/* the second tab of the hub */
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
	N_PAGES
};

#define PAGE_NO_PARENT	(-1)	/* the top of the tree: back leaves the app */
#define PAGE_HUB_TAB	(-2)	/* whichever tab of the hub was last shown */

static const int PAGE_PARENTS[N_PAGES] = {
	[PAGE_MAIN] = PAGE_NO_PARENT,
	[PAGE_APPS] = PAGE_NO_PARENT,
	[PAGE_THEME] = PAGE_HUB_TAB,
	[PAGE_DISPLAY] = PAGE_HUB_TAB,
	[PAGE_CONNECTIONS] = PAGE_HUB_TAB,
	[PAGE_WIFI] = PAGE_CONNECTIONS,
	[PAGE_MOBILE] = PAGE_CONNECTIONS,
	[PAGE_SOUND_TOUCH] = PAGE_HUB_TAB,
	[PAGE_AUDIO] = PAGE_SOUND_TOUCH,
	[PAGE_POWER] = PAGE_HUB_TAB,
	[PAGE_BATTERY] = PAGE_POWER,
	[PAGE_CPU] = PAGE_POWER,
	[PAGE_VM] = PAGE_POWER,
	[PAGE_GPU] = PAGE_POWER,
	[PAGE_KEYBOARD] = PAGE_HUB_TAB,
	[PAGE_ABOUT] = PAGE_HUB_TAB,
	[PAGE_PERF] = PAGE_POWER,
	[PAGE_DEV] = PAGE_HUB_TAB,
	[PAGE_INSTALLED_APPS] = PAGE_APPS,
	[PAGE_APP_DETAIL] = PAGE_INSTALLED_APPS,
	[PAGE_HAPTICS] = PAGE_SOUND_TOUCH,
	[PAGE_QUICK] = PAGE_HUB_TAB,
	[PAGE_APN] = PAGE_MOBILE,
	[PAGE_WIFI_JOIN] = PAGE_WIFI,
	[PAGE_WIFI_NET] = PAGE_WIFI,
	[PAGE_DATE] = PAGE_HUB_TAB,
	[PAGE_BLUETOOTH] = PAGE_CONNECTIONS,
	[PAGE_ACCENTS] = PAGE_THEME,
	[PAGE_WALLS] = PAGE_THEME,
	[PAGE_NAVIGATION] = PAGE_HUB_TAB,
	[PAGE_BOOST] = PAGE_POWER,
};

/* Pages by name: `ztsettings NAME` (Hyperscan opens one), --render NAME FILE,
 * and --list-pages for a script that wants them all. */
static const struct { const char *name; enum page page; } PAGE_NAMES[] = {
	{ "main", PAGE_MAIN },
	{ "apps", PAGE_APPS },
	{ "installed-apps", PAGE_INSTALLED_APPS },
	{ "app", PAGE_APP_DETAIL },
	{ "theme", PAGE_THEME },
	{ "accents", PAGE_ACCENTS },
	{ "wallpapers", PAGE_WALLS },
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
	{ "power", PAGE_POWER },
	{ "battery", PAGE_BATTERY },
	{ "perf", PAGE_PERF },
	{ "boost", PAGE_BOOST },
	{ "cpu", PAGE_CPU },
	{ "gpu", PAGE_GPU },
	{ "vm", PAGE_VM },
	{ "date", PAGE_DATE },
	{ "keyboard", PAGE_KEYBOARD },
	{ "dev", PAGE_DEV },
	{ "about", PAGE_ABOUT },
};
#define N_PAGE_NAMES (int)(sizeof(PAGE_NAMES) / sizeof(PAGE_NAMES[0]))

/* Where back goes from a page. */
static inline int page_parent(int page, int hub_tab) {
	int up = page >= 0 && page < N_PAGES ? PAGE_PARENTS[page] : PAGE_NO_PARENT;
	return up == PAGE_HUB_TAB ? hub_tab : up;
}

#endif
