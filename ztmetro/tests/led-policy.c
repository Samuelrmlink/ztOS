/* make check-led: who has the front LED, how it looks and how it is written. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ledd/led-policy.h"

static enum led_source win(const struct led_config *c, bool notify, enum led_charge charge, bool boost) {
	struct led_claims w = { .notify = notify, .boost = boost, .charge = charge };
	return led_winner(c, &w);
}

static int find(const struct led_write *w, int n, int channel, const char *attr, int from) {
	for (int i = from; i < n; i++) {
		if (w[i].channel == channel && !strcmp(w[i].attr, attr)) return i;
	}
	return -1;
}

static void priorities(void) {
	struct led_config c;
	led_config_defaults(&c);
	c.on[LEDSRC_BOOST] = true;
	static const enum led_charge charges[] = { LED_CHARGE_NONE, LED_CHARGE_CHARGING, LED_CHARGE_FULL, LED_CHARGE_LOW };
	/* a notification beats everything, whatever else is claimed */
	for (int b = 0; b < 2; b++) {
		for (int i = 0; i < 4; i++) assert(win(&c, true, charges[i], b) == LEDSRC_NOTIFY);
	}
	/* the battery beats the boost */
	assert(win(&c, false, LED_CHARGE_LOW, true) == LEDSRC_LOW);
	assert(win(&c, false, LED_CHARGE_CHARGING, true) == LEDSRC_CHARGING);
	assert(win(&c, false, LED_CHARGE_FULL, true) == LEDSRC_FULL);
	assert(win(&c, false, LED_CHARGE_NONE, true) == LEDSRC_BOOST);
	/* nothing to show: off */
	assert(win(&c, false, LED_CHARGE_NONE, false) == LED_NOBODY);

	/* switched off: the next one down has it */
	c.on[LEDSRC_NOTIFY] = false;
	assert(win(&c, true, LED_CHARGE_LOW, true) == LEDSRC_LOW);
	c.on[LEDSRC_LOW] = false;
	assert(win(&c, true, LED_CHARGE_LOW, true) == LEDSRC_BOOST);
	c.on[LEDSRC_BOOST] = false;
	assert(win(&c, true, LED_CHARGE_LOW, true) == LED_NOBODY);
	c.on[LEDSRC_CHARGING] = false;
	assert(win(&c, false, LED_CHARGE_CHARGING, false) == LED_NOBODY);
	assert(win(&c, false, LED_CHARGE_FULL, false) == LEDSRC_FULL);

	/* the master switch beats all of it */
	led_config_defaults(&c);
	c.enabled = false;
	for (int i = 0; i < 4; i++) assert(win(&c, true, charges[i], true) == LED_NOBODY);
}

/* The defaults are what the phone did before, but for the boost */
static void defaults(void) {
	struct led_config c;
	led_config_defaults(&c);
	assert(c.enabled);
	struct led_output o = led_output_of(&c, LEDSRC_CHARGING);
	assert(o.rgb[0] == 24 && !o.rgb[1] && !o.rgb[2] && !o.on_ms);
	o = led_output_of(&c, LEDSRC_FULL);
	assert(!o.rgb[0] && o.rgb[1] == 24 && !o.rgb[2] && !o.on_ms);
	o = led_output_of(&c, LEDSRC_LOW);
	assert(o.rgb[0] == 120 && !o.rgb[1] && !o.rgb[2]);
	assert(o.on_ms == 500 && o.off_ms == 500 && o.ramp_ms == 400);
	/* off, and white at the old level when asked for */
	assert(!c.on[LEDSRC_BOOST]);
	assert(win(&c, false, LED_CHARGE_NONE, true) == LED_NOBODY);
	o = led_output_of(&c, LEDSRC_BOOST);
	assert(o.rgb[0] == 40 && o.rgb[1] == 40 && o.rgb[2] == 40 && o.on_ms && !o.ramp_ms);
	/* no light */
	o = led_output_of(&c, LED_NOBODY);
	assert(!o.rgb[0] && !o.rgb[1] && !o.rgb[2] && !o.on_ms);
	/* a default the page can ring */
	for (int i = 0; i < N_LED_SOURCES; i++) {
		if (!LED_LOOKS[i].colour_key) continue;
		bool listed = false;
		for (int p = 0; p < LED_N_PALETTE; p++) listed |= LED_PALETTE[p] == LED_LOOKS[i].default_colour;
		assert(listed);
	}
}

static void colours(void) {
	struct led_config c;
	led_config_defaults(&c);
	/* a hue, at the source's own level: never brighter for being another colour */
	c.colour[LEDSRC_CHARGING] = 0xffffff;
	struct led_output o = led_output_of(&c, LEDSRC_CHARGING);
	assert(o.rgb[0] == 24 && o.rgb[1] == 24 && o.rgb[2] == 24);
	/* what a colour has stays lit however dim, and what it lacks stays dark */
	c.colour[LEDSRC_CHARGING] = 0x010000;
	o = led_output_of(&c, LEDSRC_CHARGING);
	assert(o.rgb[0] == 1 && !o.rgb[1] && !o.rgb[2]);
}

static void config_lines(void) {
	struct led_config c;
	led_config_defaults(&c);
	int keys = 1;
	for (int i = 0; i < N_LED_SOURCES; i++) keys += 1 + (LED_LOOKS[i].colour_key != NULL);
	assert(keys == LED_N_CONFIG_KEYS);

	assert(led_config_line(&c, "led", "off") && !c.enabled);
	assert(led_config_line(&c, "led", "on") && c.enabled);
	assert(led_config_line(&c, "led_boost", "on") && c.on[LEDSRC_BOOST]);
	assert(led_config_line(&c, "led_notify", "off") && !c.on[LEDSRC_NOTIFY]);
	assert(led_config_line(&c, "led_low_colour", "00ff00") && c.colour[LEDSRC_LOW] == 0x00ff00);
	assert(led_config_line(&c, "led_full_colour", "#0000FF") && c.colour[LEDSRC_FULL] == 0x0000ff);
	/* nonsense is ours, and changes nothing */
	assert(led_config_line(&c, "led_low_colour", "green") && c.colour[LEDSRC_LOW] == 0x00ff00);
	assert(led_config_line(&c, "led_low_colour", "ff00") && c.colour[LEDSRC_LOW] == 0x00ff00);
	assert(led_config_line(&c, "led_low_colour", "ff00ff00") && c.colour[LEDSRC_LOW] == 0x00ff00);
	assert(led_config_line(&c, "led_low_colour", "") && c.colour[LEDSRC_LOW] == 0x00ff00);
	assert(led_config_line(&c, "led_boost", "maybe") && c.on[LEDSRC_BOOST]);
	/* the shell's keys are not ours; "led_boost_colour" is nobody's */
	assert(!led_config_line(&c, "lock", "on"));
	assert(!led_config_line(&c, "led_boost_colour", "ffffff"));
}

static void claims(void) {
	assert(led_charge_parse("charging\n") == LED_CHARGE_CHARGING);
	assert(led_charge_parse("full\n") == LED_CHARGE_FULL);
	assert(led_charge_parse("low\n") == LED_CHARGE_LOW);
	assert(led_charge_parse("none\n") == LED_CHARGE_NONE);
	assert(led_charge_parse("what") == LED_CHARGE_NONE);
	assert(led_flag_parse("1\n") && !led_flag_parse("0\n") && !led_flag_parse(""));
}

static void plans(void) {
	struct led_config c;
	struct led_write w[LED_MAX_WRITES];
	led_config_defaults(&c);

	/* off: every trigger gone, no ramp, every channel dark */
	struct led_output o = led_output_of(&c, LED_NOBODY);
	int n = led_plan(&o, w);
	assert(n <= LED_MAX_WRITES);
	for (int i = 0; i < 3; i++) {
		int t = find(w, n, i, "trigger", 0), b = find(w, n, i, "brightness", 0);
		assert(t >= 0 && !strcmp(w[t].value, "none"));
		assert(b >= 0 && !strcmp(w[b].value, "0") && t < b);
		assert(find(w, n, i, "delay_on", 0) < 0);
	}

	/* steady: the ramp cleared before the level, or it fades in */
	o = led_output_of(&c, LEDSRC_CHARGING);
	n = led_plan(&o, w);
	int ramp = find(w, n, 0, "ramp", 0), level = find(w, n, 0, "brightness", 0);
	assert(ramp >= 0 && ramp < level && !strcmp(w[ramp].value, "0 0") && !strcmp(w[level].value, "24"));
	assert(find(w, n, 0, "ramp", ramp + 1) < 0);

	/* breathing, as the driver's comment has it: level, ramp, timer, delays */
	o = led_output_of(&c, LEDSRC_LOW);
	n = led_plan(&o, w);
	level = find(w, n, 0, "brightness", 0);
	ramp = find(w, n, 0, "ramp", level);
	int timer = find(w, n, 0, "trigger", 1), on = find(w, n, 0, "delay_on", 0), off = find(w, n, 0, "delay_off", 0);
	assert(!strcmp(w[level].value, "120") && !strcmp(w[ramp].value, "400 400"));
	assert(!strcmp(w[timer].value, "timer") && !strcmp(w[on].value, "500") && !strcmp(w[off].value, "500"));
	assert(level < ramp && ramp < timer && timer < on && on < off);
	/* a dark channel gets no timer: the driver would light it at a default */
	assert(find(w, n, 1, "delay_on", 0) < 0 && find(w, n, 2, "delay_on", 0) < 0);

	/* the most writes there can be: all three channels blinking and a ramp */
	c.colour[LEDSRC_NOTIFY] = 0xffffff;
	o = led_output_of(&c, LEDSRC_NOTIFY);
	n = led_plan(&o, w);
	assert(n <= LED_MAX_WRITES);
	assert(find(w, n, 2, "delay_off", 0) >= 0);

	/* a breath fits in its blink */
	for (int i = 0; i < N_LED_SOURCES; i++) {
		assert(!LED_LOOKS[i].ramp_ms || LED_LOOKS[i].ramp_ms < LED_LOOKS[i].on_ms);
		assert(!LED_LOOKS[i].ramp_ms || LED_LOOKS[i].ramp_ms <= LED_LOOKS[i].off_ms);
	}
}

int main(void) {
	priorities();
	defaults();
	colours();
	config_lines();
	claims();
	plans();
	puts("led-policy: ok");
	return 0;
}
