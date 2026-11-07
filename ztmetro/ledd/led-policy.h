/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front LED's rules: who may light it, in what order, how it looks, and
 * the order its sysfs files are written in. No I/O, so that it can be tested on
 * any machine (tests/led-policy.c). zrn-ledd works the light by these and
 * Settings draws its page from them.
 */
#ifndef ZRN_LED_POLICY_H
#define ZRN_LED_POLICY_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The claims, one small file each. Root's programs write in the first
 * directory, the session's in the second (group video). Nothing else may write
 * there: root must not follow a link that a user program left. */
#define LED_STATE_DIR	"/run/zrn-led"
#define LED_USER_DIR	LED_STATE_DIR "/user"
#define LED_SYS_DIR	"/sys/class/leds"
#define LED_CLAIM_CHARGE	"charge"	/* none, charging, full or low: zrn-charge */
#define LED_CLAIM_BOOST		"boost"		/* 0 or 1: zrnperfd */
#define LED_CLAIM_NOTIFY	"notify"	/* 0 or 1: the shell */
#define LED_POKE_SETTINGS	"settings"	/* written by Settings: read the config again */

/* Highest priority first: the first one that wants the light has it. */
enum led_source { LEDSRC_NOTIFY, LEDSRC_LOW, LEDSRC_CHARGING, LEDSRC_FULL, LEDSRC_BOOST, N_LED_SOURCES };
#define LED_NOBODY N_LED_SOURCES

enum led_charge { LED_CHARGE_NONE, LED_CHARGE_CHARGING, LED_CHARGE_FULL, LED_CHARGE_LOW };

struct led_claims {
	bool notify, boost;
	enum led_charge charge;
};

/*
 * How each looks. A colour in the config is a hue (ff0000); level is how bright
 * that is on this LED, so choosing another colour never makes it brighter. The
 * chip does all the blinking, so a lit LED costs the processor nothing: on_ms
 * and off_ms 0 is steady, and with a ramp the blink breathes (on_ms is the rise
 * and what is held after it; off_ms the fall and the dark after it).
 *
 * Low, charging and full are as zrn-charge drew them before this existed. The
 * boost blink is the chip's fastest near zrnperfd's old 100 ms strobe.
 */
static const struct led_look {
	const char *title, *key, *colour_key;
	bool default_on;
	unsigned default_colour;
	int level, on_ms, off_ms, ramp_ms;
} LED_LOOKS[N_LED_SOURCES] = {
	[LEDSRC_NOTIFY] = { "Notifications", "led_notify", "led_notify_colour", true, 0x0000ff, 120, 750, 2500, 600 },
	[LEDSRC_LOW] = { "Low battery", "led_low", "led_low_colour", true, 0xff0000, 120, 500, 500, 400 },
	[LEDSRC_CHARGING] = { "Charging", "led_charging", "led_charging_colour", true, 0xff0000, 24, 0, 0, 0 },
	[LEDSRC_FULL] = { "Battery full", "led_full", "led_full_colour", true, 0x00ff00, 24, 0, 0, 0 },
	/* off by default: a white blink at the top boost rung reads as a fault */
	[LEDSRC_BOOST] = { "RapidBoost indicator", "led_boost", NULL, false, 0xffffff, 40, 100, 200, 0 },
};
#define LED_MASTER_KEY	"led"
#define LED_N_CONFIG_KEYS 10	/* the master, five switches, four colours */

/* The colours Settings offers. A config can name any other. */
static const unsigned LED_PALETTE[] = {
	0xff0000, 0xff5000, 0xffc000, 0x00ff00, 0x00ffff, 0x0000ff, 0xff00ff, 0xffffff,
};
#define LED_N_PALETTE ((int)(sizeof(LED_PALETTE) / sizeof(LED_PALETTE[0])))

struct led_config {
	bool enabled;
	bool on[N_LED_SOURCES];
	unsigned colour[N_LED_SOURCES];
};

static inline void led_config_defaults(struct led_config *c) {
	c->enabled = true;
	for (int i = 0; i < N_LED_SOURCES; i++) {
		c->on[i] = LED_LOOKS[i].default_on;
		c->colour[i] = LED_LOOKS[i].default_colour;
	}
}

static inline bool led_parse_colour(const char *value, unsigned *out) {
	if (*value == '#') value++;
	char *end;
	unsigned long v = strtoul(value, &end, 16);
	if (end - value != 6 || *end) return false;
	*out = (unsigned)v;
	return true;
}

/* One line of the config file, as key and value. False if it is not ours. A
 * value that makes no sense leaves that setting as it was. */
static inline bool led_config_line(struct led_config *c, const char *key, const char *value) {
	if (!strcmp(key, LED_MASTER_KEY)) {
		if (!strcmp(value, "on")) c->enabled = true;
		else if (!strcmp(value, "off")) c->enabled = false;
		return true;
	}
	for (int i = 0; i < N_LED_SOURCES; i++) {
		if (!strcmp(key, LED_LOOKS[i].key)) {
			if (!strcmp(value, "on")) c->on[i] = true;
			else if (!strcmp(value, "off")) c->on[i] = false;
			return true;
		}
		if (LED_LOOKS[i].colour_key && !strcmp(key, LED_LOOKS[i].colour_key)) {
			led_parse_colour(value, &c->colour[i]);
			return true;
		}
	}
	return false;
}

/* What a claim file says. Unknown words claim nothing. */
static inline enum led_charge led_charge_parse(const char *text) {
	if (!strncmp(text, "charging", 8)) return LED_CHARGE_CHARGING;
	if (!strncmp(text, "full", 4)) return LED_CHARGE_FULL;
	if (!strncmp(text, "low", 3)) return LED_CHARGE_LOW;
	return LED_CHARGE_NONE;
}

static inline bool led_flag_parse(const char *text) {
	return text[0] == '1';
}

/* The source that has the light: the top of the list that claims it and is
 * allowed. LED_NOBODY when there is nothing to show. */
static inline enum led_source led_winner(const struct led_config *c, const struct led_claims *w) {
	if (!c->enabled) return LED_NOBODY;
	if (w->notify && c->on[LEDSRC_NOTIFY]) return LEDSRC_NOTIFY;
	if (w->charge == LED_CHARGE_LOW && c->on[LEDSRC_LOW]) return LEDSRC_LOW;
	if (w->charge == LED_CHARGE_CHARGING && c->on[LEDSRC_CHARGING]) return LEDSRC_CHARGING;
	if (w->charge == LED_CHARGE_FULL && c->on[LEDSRC_FULL]) return LEDSRC_FULL;
	if (w->boost && c->on[LEDSRC_BOOST]) return LEDSRC_BOOST;
	return LED_NOBODY;
}

struct led_output {
	int rgb[3];
	int on_ms, off_ms, ramp_ms;
};

/* A colour's channel at a level: anything the colour has stays lit, however dim. */
static inline int led_channel(unsigned colour, int shift, int level) {
	int c = (colour >> shift) & 0xff;
	int v = c * level / 255;
	return c && !v ? 1 : v;
}

static inline struct led_output led_output_of(const struct led_config *c, enum led_source who) {
	struct led_output o;
	memset(&o, 0, sizeof(o));
	if (who >= LED_NOBODY) return o;
	const struct led_look *l = &LED_LOOKS[who];
	for (int i = 0; i < 3; i++) o.rgb[i] = led_channel(c->colour[who], 16 - 8 * i, l->level);
	o.on_ms = l->on_ms;
	o.off_ms = l->off_ms;
	o.ramp_ms = l->ramp_ms;
	return o;
}

static inline bool led_output_same(const struct led_output *a, const struct led_output *b) {
	return !memcmp(a, b, sizeof(*a));
}

/* The writes that show an output, in order. `required` is false for what a
 * light without that file does without. */
#define LED_MAX_WRITES 20
struct led_write {
	int channel;
	const char *attr;
	char value[32];
	bool required;
};

static const char *const LED_CHANNELS[3] = { "red:status", "green:status", "blue:status" };

static inline void led_plan_add(struct led_write *w, int *n, int channel, const char *attr, long value, const char *text, bool required) {
	w[*n].channel = channel;
	w[*n].attr = attr;
	if (text) snprintf(w[*n].value, sizeof(w[*n].value), "%s", text);
	else snprintf(w[*n].value, sizeof(w[*n].value), "%ld", value);
	w[*n].required = required;
	(*n)++;
}

/*
 * Whatever was showing goes first, and the ramp with it: with a ramp left set a
 * steady light would fade in. Then, as the driver wants it (leds/zrn-led.c), the
 * levels, the ramp the channels share, and the timer on each lit channel.
 * The period is shared too, so the three blink as one.
 */
static inline int led_plan(const struct led_output *o, struct led_write *w) {
	int n = 0;
	char ramp[24];
	for (int i = 0; i < 3; i++) led_plan_add(w, &n, i, "trigger", 0, "none", false);
	led_plan_add(w, &n, 0, "ramp", 0, "0 0", false);
	for (int i = 0; i < 3; i++) led_plan_add(w, &n, i, "brightness", o->rgb[i], NULL, true);
	if (!o->on_ms) return n;
	if (o->ramp_ms) {
		snprintf(ramp, sizeof(ramp), "%d %d", o->ramp_ms, o->ramp_ms);
		led_plan_add(w, &n, 0, "ramp", 0, ramp, false);
	}
	for (int i = 0; i < 3; i++) {
		if (!o->rgb[i]) continue;
		led_plan_add(w, &n, i, "trigger", 0, "timer", true);
		led_plan_add(w, &n, i, "delay_on", o->on_ms, NULL, true);
		led_plan_add(w, &n, i, "delay_off", o->off_ms, NULL, true);
	}
	return n;
}

#endif
