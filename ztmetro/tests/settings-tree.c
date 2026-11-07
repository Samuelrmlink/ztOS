/* make check-settings-tree: back goes up the tree from every page, and out at
 * the top of it. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../settings/page-tree.h"

int main(void) {
	/* the old ladder's answers */
	assert(page_parent(PAGE_MAIN, PAGE_MAIN) == PAGE_NO_PARENT);
	assert(page_parent(PAGE_APPS, PAGE_APPS) == PAGE_NO_PARENT);
	assert(page_parent(PAGE_APP_DETAIL, PAGE_MAIN) == PAGE_INSTALLED_APPS);
	assert(page_parent(PAGE_INSTALLED_APPS, PAGE_MAIN) == PAGE_APPS);
	assert(page_parent(PAGE_WIFI_NET, PAGE_MAIN) == PAGE_WIFI);
	assert(page_parent(PAGE_WIFI_JOIN, PAGE_MAIN) == PAGE_WIFI);
	assert(page_parent(PAGE_ACCENTS, PAGE_MAIN) == PAGE_THEME);
	assert(page_parent(PAGE_WALLS, PAGE_MAIN) == PAGE_THEME);
	assert(page_parent(PAGE_APN, PAGE_MAIN) == PAGE_MOBILE);
	assert(page_parent(PAGE_WIFI, PAGE_MAIN) == PAGE_CONNECTIONS);
	assert(page_parent(PAGE_BLUETOOTH, PAGE_MAIN) == PAGE_CONNECTIONS);
	assert(page_parent(PAGE_MOBILE, PAGE_MAIN) == PAGE_CONNECTIONS);
	assert(page_parent(PAGE_AUDIO, PAGE_MAIN) == PAGE_SOUND_TOUCH);
	assert(page_parent(PAGE_HAPTICS, PAGE_MAIN) == PAGE_SOUND_TOUCH);
	/* the rest go to the tab they were opened from */
	assert(page_parent(PAGE_PERF, PAGE_APPS) == PAGE_APPS);
	assert(page_parent(PAGE_PERF, PAGE_MAIN) == PAGE_MAIN);

	/* every page gets to the top in at most 3 steps, whichever tab it was */
	for (int hub = PAGE_MAIN; hub <= PAGE_APPS; hub += PAGE_APPS - PAGE_MAIN) {
		for (int p = 0; p < N_PAGES; p++) {
			int steps = 0, at = p;
			while (page_parent(at, hub) != PAGE_NO_PARENT) {
				at = page_parent(at, hub);
				assert(at >= 0 && at < N_PAGES);
				assert(++steps <= 3);
			}
			assert(at == PAGE_MAIN || at == PAGE_APPS);
		}
	}
	/* every page has a name, and no name means two pages */
	for (int p = 0; p < N_PAGES; p++) {
		int named = 0;
		for (int n = 0; n < N_PAGE_NAMES; n++) named += PAGE_NAMES[n].page == (enum page)p;
		assert(named >= 1);
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
