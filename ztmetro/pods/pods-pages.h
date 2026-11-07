// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The AirPods pages, for whichever app has them as its own: the ztPods app (ztpods.c),
 * and Settings under its Bluetooth page. Four pages in a row from `first`, in this
 * order: status, listening, controls, about. The host calls these from its own zmc_app
 * functions for those pages. Their actions are from PODS_ACTIONS up.
 */
#ifndef PODS_PAGES_H
#define PODS_PAGES_H
#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>

#define PODS_ACTIONS 200000
#define PODS_ACTIONS_END 210000
enum { PODS_STATUS, PODS_LISTEN, PODS_CONTROLS, PODS_ABOUT, PODS_TABS };

/* bluetooth: what "open Bluetooth" on the status page does */
void pods_pages_setup(int first, void (*bluetooth)(void));
void pods_show(const char *mac);	/* which pair; NULL: the first */
void pods_render_as_of(long when);	/* --render: the file's own time, for stable pictures */
double pods_render(cairo_t *cr, int page);
void pods_activate(int action, int arg);
void pods_slide(int action, int percent, bool done);
bool pods_back(void);
void pods_text(const char *typed);
void pods_arrived(int page);
#endif
