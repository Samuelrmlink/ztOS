// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The Sound pages, for whichever app has them as its own pages: the Sound app
 * (ztsound.c), and Settings under "ringtones + sounds". Back and the task switcher
 * are the host's: nothing here starts another app.
 *
 * The host gives them four pages in a row, from `first`, in this order: equalizer,
 * effects, output, apps. It calls these from its own zmc_app functions for those
 * pages. Their actions are from SOUND_ACTIONS up, so that they cannot be the host's.
 */
#ifndef SOUND_PAGES_H
#define SOUND_PAGES_H
#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>

#define SOUND_ACTIONS 100000
#define SOUND_ACTIONS_END 110000
enum { SOUND_EQ, SOUND_FX, SOUND_OUT, SOUND_APPS, SOUND_TABS };

/* two_by_two: equalizer with effects and output with apps, each pair a pivot of two;
 * else all four tabs go round. to_a_file: --render, so the plot is drawn with the page.
 * shown: the page opening first. */
void sound_pages_setup(int first, bool two_by_two, bool to_a_file, int shown);
double sound_render(cairo_t *cr, int page);
void sound_activate(int action, int arg);
void sound_slide(int action, int percent, bool done);
/* something of theirs to put away (a picker, the preset's name): true if it was */
bool sound_back(void);
void sound_text(const char *typed);
void sound_overlay(int page);
bool sound_touch(int phase, int id, double x, double y, uint32_t time);
void sound_arrived(int page);
#endif
