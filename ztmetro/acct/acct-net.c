// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztacctd: bytes per socket. (Comes in its own commit.)
 */
#include "acctd.h"

bool net_start(void) { return false; }
void net_tick(double dt) { (void)dt; }
void net_status(FILE *f) { fprintf(f, "\"net\":{\"up\":0},"); }
bool net_up(void) { return false; }
int net_default_class(void) { return NET_WIFI; }
