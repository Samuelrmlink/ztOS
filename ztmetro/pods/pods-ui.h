// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * What the AirPods front ends show, shared: ztMETRO's pages (pods-pages.c) and the
 * desktop app (qt/). The settings rows, the listening modes, the colours, and the text
 * made out of the state's words. No I/O, no clock, no drawing; C, and C++ too.
 */
#ifndef PODS_UI_H
#define PODS_UI_H
#include <stdio.h>
#include <string.h>

enum pods_ui_page { PODS_UI_STATUS, PODS_UI_LISTEN, PODS_UI_CONTROLS, PODS_UI_ABOUT };
enum pods_row_kind { PODS_R_SWITCH, PODS_R_SLIDER, PODS_R_PICKER };

/* A setting's row, shown if the state's "visible" has its key. Sliders are 0-100. */
struct pods_row {
	const char *key, *title;
	enum pods_row_kind kind;
	int page;
	const char *const *options;	/* PODS_R_PICKER */
	const int *values;
	int count;
};

static const char *const PODS_O_MIC[] = { "Automatic", "Always right", "Always left" };
static const int PODS_V_012[] = { 0, 1, 2 };
static const char *const PODS_O_HOLD[] = { "Noise control", "Siri" };
static const int PODS_V_HOLD[] = { 1, 5 };
static const char *const PODS_O_SPEED[] = { "Default", "Slower", "Slowest" };
static const char *const PODS_O_SWIPE[] = { "Default", "Longer", "Longest" };

static const struct pods_row PODS_ROWS[] = {
	{ "conversation", "Conversation Awareness", PODS_R_SWITCH, PODS_UI_LISTEN, NULL, NULL, 0 },
	{ "adaptive_strength", "Adaptive strength", PODS_R_SLIDER, PODS_UI_LISTEN, NULL, NULL, 0 },
	{ "personalized_volume", "Personalized Volume", PODS_R_SWITCH, PODS_UI_LISTEN, NULL, NULL, 0 },
	{ "one_bud_anc", "ANC with one AirPod", PODS_R_SWITCH, PODS_UI_LISTEN, NULL, NULL, 0 },
	{ "hold_left", "Press and hold, left", PODS_R_PICKER, PODS_UI_CONTROLS, PODS_O_HOLD, PODS_V_HOLD, 2 },
	{ "hold_right", "Press and hold, right", PODS_R_PICKER, PODS_UI_CONTROLS, PODS_O_HOLD, PODS_V_HOLD, 2 },
	{ "press_speed", "Press speed", PODS_R_PICKER, PODS_UI_CONTROLS, PODS_O_SPEED, PODS_V_012, 3 },
	{ "hold_duration", "Press and hold duration", PODS_R_PICKER, PODS_UI_CONTROLS, PODS_O_SPEED, PODS_V_012, 3 },
	{ "swipe_volume", "Swipe for volume", PODS_R_SWITCH, PODS_UI_CONTROLS, NULL, NULL, 0 },
	{ "swipe_length", "Swipe length", PODS_R_PICKER, PODS_UI_CONTROLS, PODS_O_SWIPE, PODS_V_012, 3 },
	{ "ear_detection", "Automatic ear detection", PODS_R_SWITCH, PODS_UI_CONTROLS, NULL, NULL, 0 },
	{ "mic", "Microphone", PODS_R_PICKER, PODS_UI_CONTROLS, PODS_O_MIC, PODS_V_012, 3 },
	{ "sleep_detection", "Pause when you fall asleep", PODS_R_SWITCH, PODS_UI_CONTROLS, NULL, NULL, 0 },
	{ "chime_volume", "Chime volume", PODS_R_SLIDER, PODS_UI_CONTROLS, NULL, NULL, 0 },
	{ "case_tone", "Case sounds", PODS_R_SWITCH, PODS_UI_CONTROLS, NULL, NULL, 0 },
	{ "case_tone_volume", "Case sounds volume", PODS_R_SLIDER, PODS_UI_CONTROLS, NULL, NULL, 0 },
	/* not shown: "connect_auto" (0x20). Only named in LibrePods' table, and it may
	 * change how the pods reconnect. pods-model.h keeps it for when it's been tried. */
};
#define PODS_N_ROWS ((int)(sizeof(PODS_ROWS) / sizeof(PODS_ROWS[0])))

/* The listening modes, as `set MAC listening_mode NAME` has them; bit: in mode_cycle.
 * A mode_cycle of fewer than two bits is refused by the daemon. */
struct pods_mode { const char *name, *label; int bit; };
static const struct pods_mode PODS_MODES[] = {
	{ "off", "Off", 1 }, { "anc", "Noise Cancellation", 2 },
	{ "transparency", "Transparency", 4 }, { "adaptive", "Adaptive", 8 },
};
#define PODS_N_MODES 4

/* the state's "color" */
static const char *const PODS_COLOURS[] = { "White", "Black", "Red", "Blue", "Pink", "Gray", "Silver", "Gold", "Rose Gold",
	"Space Gray", "Dark Blue", "Light Blue", "Yellow" };
#define PODS_N_COLOURS ((int)(sizeof(PODS_COLOURS) / sizeof(PODS_COLOURS[0])))

/* "ear": left and right. NULL for one not known. */
static inline const char *pods_place_text(const char *place) {
	if (!strcmp(place, "in_ear")) return "in your ear";
	if (!strcmp(place, "out")) return "out";
	if (!strcmp(place, "case")) return "in the open case";
	if (!strcmp(place, "case_closed")) return "in the closed case";
	return NULL;
}

/* What a broadcast says of who has them: "host" and "connection". here: "this phone" */
static inline void pods_where_in_use(char *out, size_t size, const char *host, long conn, const char *here) {
	const char *doing = conn == 5 ? ", playing music" : conn == 6 ? ", on a call" : conn == 7 ? ", ringing" : "";
	if (!strcmp(host, "other")) snprintf(out, size, "In use by another device%s", doing);
	else if (!strcmp(host, "this")) snprintf(out, size, "Not connected. Last used with %s%s", here, doing);
	else if (!strcmp(host, "none")) snprintf(out, size, "Not connected to anything");
	else snprintf(out, size, "Not connected to %s", here);
}

/* How old a reading is, given its age in seconds */
static inline void pods_age_text(char *out, size_t size, long s) {
	if (s < 0) s = 0;
	if (s < 90) snprintf(out, size, "just now");
	else if (s < 5400) snprintf(out, size, "%ld minutes ago", (s + 30) / 60);
	else if (s < 129600) snprintf(out, size, "%ld hours ago", (s + 1800) / 3600);
	else snprintf(out, size, "%ld days ago", (s + 43200) / 86400);
}
#endif
