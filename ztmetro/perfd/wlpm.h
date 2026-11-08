// SPDX-License-Identifier: GPL-2.0-or-later
/* wlpm: the Wi-Fi chip's screen-off power settings, and its log (see wlpm.c) */
#ifndef WLPM_H
#define WLPM_H
#include <stdbool.h>
#include <stdio.h>

/* what zrnperfd's commands set: wifi-suspend, wifi-filter, wifi-listen */
extern bool wlpm_suspend_enabled, wlpm_filter_enabled;
extern int wlpm_listen_ms;

int wlpm_open(void);		/* the nl80211 event socket to poll, or -1 */
void wlpm_event(void);		/* it is readable: connects, roams, drops */
void wlpm_screen(bool on);	/* screen-off settings in, or back out */
void wlpm_reconfigure(void);	/* a setting changed: apply it now */
int wlpm_timeout(void);		/* ms until wlpm_tick wants to run, -1: never */
void wlpm_tick(void);		/* a periodic line, a delayed reapply */
void wlpm_status(FILE *f);
#endif
