/* make check-settings-tree: back goes up the tree from every page, and out at
 * the top of it. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../settings/page-tree.h"

int main(void) {
	static const struct { int page, parent; } tree[] = {
		{ PAGE_CONNECTIONS, PAGE_MAIN }, { PAGE_THEME, PAGE_MAIN }, { PAGE_DISPLAY, PAGE_MAIN },
		{ PAGE_SOUND_TOUCH, PAGE_MAIN }, { PAGE_POWER, PAGE_MAIN }, { PAGE_INSTALLED_APPS, PAGE_MAIN },
		{ PAGE_SYSTEM, PAGE_MAIN }, { PAGE_ANDROID, PAGE_MAIN },
		{ PAGE_WIFI, PAGE_CONNECTIONS }, { PAGE_BLUETOOTH, PAGE_CONNECTIONS }, { PAGE_MOBILE, PAGE_CONNECTIONS },
		{ PAGE_WIFI_NET, PAGE_WIFI }, { PAGE_WIFI_JOIN, PAGE_WIFI }, { PAGE_APN, PAGE_MOBILE },
		{ PAGE_ACCENTS, PAGE_THEME }, { PAGE_ANIMATION, PAGE_THEME }, { PAGE_STYLE, PAGE_THEME }, { PAGE_TILES, PAGE_THEME }, { PAGE_LOCK, PAGE_THEME }, { PAGE_NAVIGATION, PAGE_THEME }, { PAGE_QUICK, PAGE_THEME },
		{ PAGE_AUDIO, PAGE_SOUND_TOUCH }, { PAGE_HAPTICS, PAGE_SOUND_TOUCH }, { PAGE_LED, PAGE_SOUND_TOUCH },
		{ PAGE_BATTERY, PAGE_POWER }, { PAGE_PERF, PAGE_POWER }, { PAGE_BOOST, PAGE_POWER },
		{ PAGE_CPU, PAGE_POWER }, { PAGE_GPU, PAGE_POWER }, { PAGE_VM, PAGE_POWER },
		{ PAGE_APP_DETAIL, PAGE_INSTALLED_APPS }, { PAGE_APP_SCALE, PAGE_INSTALLED_APPS },
		{ PAGE_DATE, PAGE_SYSTEM }, { PAGE_KEYBOARD, PAGE_SYSTEM }, { PAGE_DEV, PAGE_SYSTEM }, { PAGE_ABOUT, PAGE_SYSTEM },
		/* what used to start an app of its own, and a new entry in the task switcher */
		{ PAGE_SOUND_EQ, PAGE_AUDIO }, { PAGE_SOUND_FX, PAGE_AUDIO }, { PAGE_SOUND_OUT, PAGE_AUDIO }, { PAGE_SOUND_APPS, PAGE_AUDIO },
		{ PAGE_PODS_STATUS, PAGE_BLUETOOTH }, { PAGE_PODS_LISTEN, PAGE_BLUETOOTH },
		{ PAGE_PODS_CONTROLS, PAGE_BLUETOOTH }, { PAGE_PODS_ABOUT, PAGE_BLUETOOTH },
		{ PAGE_DEVTEST, PAGE_DEV }, { PAGE_DEVTEST_LOGS, PAGE_DEV },
		{ PAGE_DEVTEST_SYSTEM, PAGE_DEVTEST }, { PAGE_DEVTEST_SENSORS, PAGE_DEVTEST }, { PAGE_DEVTEST_HARDWARE, PAGE_DEVTEST },
		{ PAGE_DEVTEST_ANIM, PAGE_DEVTEST }, { PAGE_DEVTEST_PREVIEW, PAGE_DEVTEST_ANIM },
		{ PAGE_DEVTEST_TEMPS, PAGE_DEVTEST }, { PAGE_DEVTEST_CHARGING, PAGE_DEVTEST },
		{ PAGE_DEVTEST_JOURNAL, PAGE_DEVTEST_LOGS }, { PAGE_DEVTEST_KERNEL, PAGE_DEVTEST_LOGS }, { PAGE_DEVTEST_SHELL, PAGE_DEVTEST_LOGS },
	};
	for (size_t i = 0; i < sizeof(tree) / sizeof(tree[0]); i++) {
		assert(page_parent(tree[i].page, WALL_TARGET_TILES) == tree[i].parent);
		assert(page_parent(tree[i].page, WALL_TARGET_LOCK) == tree[i].parent);
	}
	assert(page_parent(PAGE_MAIN, WALL_TARGET_TILES) == PAGE_NO_PARENT);

	/* the picker has two parents, by whose photo it is choosing */
	assert(page_parent(PAGE_WALLS, WALL_TARGET_TILES) == PAGE_TILES);
	assert(page_parent(PAGE_WALLS, WALL_TARGET_LOCK) == PAGE_LOCK);

	/* the modules' pages are in their order, for page_is_*() and their first-page sums */
	assert(PAGE_SOUND_FX == PAGE_SOUND_EQ + 1 && PAGE_SOUND_APPS == PAGE_SOUND_EQ + 3);
	assert(PAGE_PODS_ABOUT == PAGE_PODS_STATUS + 3);
	assert(PAGE_DEVTEST_LOGS == PAGE_DEVTEST + 7 && PAGE_DEVTEST_PREVIEW == PAGE_DEVTEST + 9 && PAGE_DEVTEST_CHARGING == PAGE_DEVTEST + 11);
	assert(page_is_sound(PAGE_SOUND_APPS) && !page_is_sound(PAGE_AUDIO) && !page_is_sound(PAGE_LED));
	assert(page_is_pods(PAGE_PODS_ABOUT) && !page_is_pods(PAGE_BLUETOOTH));
	assert(page_is_devtest(PAGE_DEVTEST_PREVIEW) && !page_is_devtest(PAGE_DEV));

	/* every page gets to the top in at most 5 steps, and only the top has no parent */
	for (int target = WALL_TARGET_TILES; target <= WALL_TARGET_LOCK; target++) {
		for (int p = 0; p < N_PAGES; p++) {
			int steps = 0, at = p;
			while (page_parent(at, target) != PAGE_NO_PARENT) {
				at = page_parent(at, target);
				assert(at >= 0 && at < N_PAGES);
				assert(++steps <= 5);
			}
			assert(at == PAGE_MAIN);
		}
	}
	/* every page has a name, and no name means two pages */
	for (int p = 0; p < N_PAGES; p++) {
		int named = 0;
		for (int n = 0; n < N_PAGE_NAMES; n++) named += PAGE_NAMES[n].page == (enum page)p;
		assert(named >= 1);
	}
	for (int n = 0; n < N_PAGE_NAMES; n++) {
		if (!strcmp(PAGE_NAMES[n].name, "apps")) assert(PAGE_NAMES[n].page == PAGE_INSTALLED_APPS);
		if (!strcmp(PAGE_NAMES[n].name, "customization") || !strcmp(PAGE_NAMES[n].name, "theme")) {
			assert(PAGE_NAMES[n].page == PAGE_THEME);
		}
	}
	for (int a = 0; a < N_PAGE_NAMES; a++) {
		for (int b = a + 1; b < N_PAGE_NAMES; b++) assert(strcmp(PAGE_NAMES[a].name, PAGE_NAMES[b].name));
	}
	/* what the shell's search and people's scripts already use */
	static const char *const old[] = { "date", "main", "apps", "installed-apps", "theme", "accents",
		"wallpapers", "display", "wifi", "bluetooth", "mobile", "audio", "battery", "cpu", "vm", "gpu",
		"keyboard", "about", "perf", "boost", "dev", "haptics", "quick", "apn", "wifijoin", "wifinet", "app-scale",
		"led", "equalizer", "effects", "audio-output", "app-volume", "airpods", "devtest" };
	for (size_t i = 0; i < sizeof(old) / sizeof(old[0]); i++) {
		int found = 0;
		for (int n = 0; n < N_PAGE_NAMES; n++) found += !strcmp(PAGE_NAMES[n].name, old[i]);
		assert(found == 1);
	}
	puts("PASS: back goes up the tree from every page");
	return 0;
}
