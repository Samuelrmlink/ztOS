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
		{ PAGE_SYSTEM, PAGE_MAIN },
		{ PAGE_WIFI, PAGE_CONNECTIONS }, { PAGE_BLUETOOTH, PAGE_CONNECTIONS }, { PAGE_MOBILE, PAGE_CONNECTIONS },
		{ PAGE_WIFI_NET, PAGE_WIFI }, { PAGE_WIFI_JOIN, PAGE_WIFI }, { PAGE_APN, PAGE_MOBILE },
		{ PAGE_ACCENTS, PAGE_THEME }, { PAGE_ANIMATION, PAGE_THEME }, { PAGE_STYLE, PAGE_THEME }, { PAGE_NAVIGATION, PAGE_THEME }, { PAGE_QUICK, PAGE_THEME },
		{ PAGE_AUDIO, PAGE_SOUND_TOUCH }, { PAGE_HAPTICS, PAGE_SOUND_TOUCH },
		{ PAGE_BATTERY, PAGE_POWER }, { PAGE_PERF, PAGE_POWER }, { PAGE_BOOST, PAGE_POWER },
		{ PAGE_CPU, PAGE_POWER }, { PAGE_GPU, PAGE_POWER }, { PAGE_VM, PAGE_POWER },
		{ PAGE_APP_DETAIL, PAGE_INSTALLED_APPS },
		{ PAGE_DATE, PAGE_SYSTEM }, { PAGE_KEYBOARD, PAGE_SYSTEM }, { PAGE_DEV, PAGE_SYSTEM }, { PAGE_ABOUT, PAGE_SYSTEM },
	};
	for (size_t i = 0; i < sizeof(tree) / sizeof(tree[0]); i++) {
		assert(page_parent(tree[i].page) == tree[i].parent);
	}
	assert(page_parent(PAGE_MAIN) == PAGE_NO_PARENT);

	/* every page gets to the top in at most 3 steps, and only the top has no parent */
	for (int p = 0; p < N_PAGES; p++) {
		int steps = 0, at = p;
		while (page_parent(at) != PAGE_NO_PARENT) {
			at = page_parent(at);
			assert(at >= 0 && at < N_PAGES);
			assert(++steps <= 3);
		}
		assert(at == PAGE_MAIN);
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
		"keyboard", "about", "perf", "boost", "dev", "haptics", "quick", "apn", "wifijoin", "wifinet" };
	for (size_t i = 0; i < sizeof(old) / sizeof(old[0]); i++) {
		int found = 0;
		for (int n = 0; n < N_PAGE_NAMES; n++) found += !strcmp(PAGE_NAMES[n].name, old[i]);
		assert(found == 1);
	}
	puts("PASS: back goes up the tree from every page");
	return 0;
}
