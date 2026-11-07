// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ZrnDevTest's pages, for whichever app has them as its own: Settings under
 * developer, and the zrndevtest app. Ten pages in a row from `first`, in the order of
 * DEVTEST_PAGE_NAMES. Their actions are from DEVTEST_ACTIONS up.
 */
#ifndef DEVTEST_PAGES_H
#define DEVTEST_PAGES_H
#include <cairo.h>
#include <stdbool.h>

#define DEVTEST_ACTIONS 300000
#define DEVTEST_ACTIONS_END 310000
#define DEVTEST_PAGES 10

void devtest_pages_setup(int first, int anim, long anim_ms);
double devtest_render(cairo_t *cr, int page);
void devtest_activate(int action, int arg);
void devtest_arrived(int page);
void devtest_overlay(int page);
/* up from a page: its hub's two tabs go to hub_parent */
int devtest_parent(int page, int hub_parent);
void devtest_leave(void);	/* a page's stream and timer, on the way out of it */
#endif
