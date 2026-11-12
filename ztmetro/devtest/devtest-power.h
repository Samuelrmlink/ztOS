// SPDX-License-Identifier: GPL-2.0-or-later
/* ZrnDevTest's temperatures and charging pages: devtest-power.c. */
#ifndef DEVTEST_POWER_H
#define DEVTEST_POWER_H
#include <cairo.h>

double devtest_draw_temps(cairo_t *cr);
double devtest_draw_charging(cairo_t *cr);
#endif
