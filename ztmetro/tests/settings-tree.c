/* make check-settings-tree: back goes up the tree from every page, and out at
 * the top of it. */
#include <assert.h>
#include <stdio.h>
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
	puts("PASS: back goes up the tree from every page");
	return 0;
}
