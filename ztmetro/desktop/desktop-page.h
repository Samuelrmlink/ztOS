// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Settings' "desktop mode" page (ztmetro/desktop/DESIGN.md): whether the
 * desktop starts by itself when a monitor comes, what happens to its apps
 * when the monitor goes, the virtual screen's size. Its actions are from
 * DESKTOP_ACTIONS up. The settings are ~/.config/ztmetro/desktop, which the
 * shell reads at start and on SIGHUP.
 */
#ifndef DESKTOP_PAGE_H
#define DESKTOP_PAGE_H
#include <cairo.h>

#define DESKTOP_ACTIONS 400000
#define DESKTOP_ACTIONS_END 410000

double desktop_page_render(cairo_t *cr);
void desktop_page_activate(int action, int arg);
void desktop_page_arrived(void);
/* the detail under the page's entry on the display page */
const char *desktop_page_summary(void);
#endif
