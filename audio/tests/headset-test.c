// SPDX-License-Identifier: GPL-2.0
/*
 * Host test of audio/driver/zrn-headset-logic.h: the register values the
 * driver writes for the button levels must be the stock phone's, and the
 * readings must decode as stock's extcon-madera.c decodes them.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -o /tmp/headset-test audio/tests/headset-test.c && /tmp/headset-test
 */
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include "../driver/zrn-headset-logic.h"

static int fails;

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

/* a Mic Detect 1 Control 3 value for a press in range r (0-based), as the codec reports it */
static unsigned int micd_press(int r)
{
	return HS_MICD_VALID | HS_MICD_STS | (1u << (r + 2));
}

int main(void)
{
	struct hs_level_regs r;
	int i;

	/* 1. The level registers: what stock's driver writes for stock's ranges
	 * (worked by hand from madera_extcon_add_micd_levels: thresholds 37, 43,
	 * 51, 60, 63 of madera_micd_levels, the rest 0x3f). */
	CHECK(hs_compute_levels(&r) == 0, "levels computed");
	CHECK(r.level[3] == 0x252b, "0x2a9 = %#x, want 0x252b", r.level[3]);
	CHECK(r.level[2] == 0x333c, "0x2a8 = %#x, want 0x333c", r.level[2]);
	CHECK(r.level[1] == 0x3f3f, "0x2a7 = %#x, want 0x3f3f", r.level[1]);
	CHECK(r.level[0] == 0x3f3f, "0x2a6 = %#x, want 0x3f3f", r.level[0]);
	CHECK(r.lvl_sel == 0x9f, "0x2a4 LVL_SEL = %#x, want 0x9f", r.lvl_sel);
	CHECK(hs_micd_levels[hs_level_index(116)] == 116, "116 ohm is a level");
	CHECK(hs_micd_levels[hs_level_index(752)] == 752, "752 ohm is a level");
	CHECK(hs_level_index(40000) == -1, "beyond the table");

	/* 2. Insertion: ADC value to ohms to TRS or TRRS. */
	CHECK(hs_adc_ohms(0) == 3, "adc 0 = 3 ohm");
	CHECK(hs_adc_ohms(63) == 1257, "adc 63 = 1257 ohm");
	CHECK(hs_adc_ohms(64) == 30000, "adc 64 = open (30k)");
	CHECK(hs_adc_ohms(0x7f) == HS_OPEN_OHM, "adc 127 = open");
	CHECK(hs_adc_ohms(0xff80) == 3, "only bits 6:0 count");
	CHECK(hs_classify(hs_adc_ohms(0)) == HS_HEADPHONES, "a shorted sleeve: 3-pole headphones");
	CHECK(hs_classify(hs_adc_ohms(20)) == HS_HEADPHONES, "52 ohm: headphones (a button held, or OMTP)");
	CHECK(hs_classify(hs_adc_ohms(63)) == HS_HEADPHONES, "1257 ohm is still under a microphone");
	CHECK(hs_classify(1258) == HS_HEADSET, "1258 ohm: a microphone");
	CHECK(hs_classify(2200) == HS_HEADSET, "a typical electret: 2.2k");
	CHECK(hs_classify(hs_adc_ohms(64)) == HS_HEADSET, "open: a 4-pole plug, as stock");
	CHECK(hs_classify(HS_OPEN_OHM) == HS_HEADSET, "open: a 4-pole plug, as stock");
	CHECK(hs_classify(-5) == HS_HEADPHONES, "no reading: headphones, no microphone");

	/* 3. Buttons. */
	CHECK(hs_button_range(0) == -1, "not valid yet");
	CHECK(hs_button_range(HS_MICD_VALID) == 0, "valid, nothing pressed");
	CHECK(hs_button_range(HS_MICD_VALID | HS_MICD_STS) == 0, "above every level: the mic");
	CHECK(hs_range_key(hs_button_range(micd_press(0))) == KEY_PLAYPAUSE, "range 0 is play/pause");
	CHECK(hs_range_key(hs_button_range(micd_press(1))) == KEY_VOICECOMMAND, "range 1 is voice");
	CHECK(hs_range_key(hs_button_range(micd_press(2))) == KEY_VOLUMEUP, "range 2 is volume up");
	CHECK(hs_range_key(hs_button_range(micd_press(3))) == KEY_VOLUMEDOWN, "range 3 is volume down");
	CHECK(hs_button_range(micd_press(4)) == 5 && hs_range_key(5) == 0, "range 4: measured, no key");
	CHECK(hs_button_range(micd_press(7)) == 0, "a level with no range: nothing");
	/* several level bits: the lowest wins, as madera_micd_read's ffs() */
	CHECK(hs_range_key(hs_button_range(micd_press(2) | micd_press(3))) == KEY_VOLUMEUP, "lowest level wins");
	CHECK(hs_range_key(0) == 0 && hs_range_key(99) == 0, "no key out of range");
	/* every key the input device must declare */
	for (i = 0; i < HS_NUM_RANGES; i++)
		CHECK(hs_ranges[i].key == 0 || hs_ranges[i].key < KEY_MAX, "key %d", i);
	for (i = 1; i < HS_NUM_RANGES; i++)
		CHECK(hs_ranges[i].max > hs_ranges[i - 1].max, "ranges sorted");

	/* 4. Headphone level: never above the ceiling, whatever is asked. */
	CHECK(HS_HP_CEIL_DB <= -20, "ceiling is at most -20 dB");
	CHECK(hs_hp_clamp_db(0, 0) == HS_HP_CEIL_DB, "0 dB asked, ceiling given");
	CHECK(hs_hp_clamp_db(31, 31) == HS_HP_CEIL_DB, "+31 dB asked, ceiling given");
	CHECK(hs_hp_clamp_db(-10, -30) == -30, "a lower hp_max_db wins");
	CHECK(hs_hp_clamp_db(-100, 0) == HS_HP_FLOOR_DB, "floor");
	CHECK(hs_hp_clamp_db(HS_HP_DEFAULT_DB, 0) == HS_HP_DEFAULT_DB, "default within range");
	CHECK(HS_HP_DEFAULT_DB < HS_HP_CEIL_DB, "starts under the ceiling");
	CHECK(hs_hp_vol_field(0) == 0x80, "0 dB = 0x80");
	CHECK(hs_hp_vol_field(-20) == 0x58, "-20 dB = 0x58");
	CHECK(hs_hp_vol_field(-40) == 0x30, "-40 dB = 0x30");
	CHECK(hs_hp_vol_field(-64) == 0x00, "-64 dB = 0x00");
	for (i = -200; i <= 200; i++) {
		unsigned int f = hs_hp_vol_field(hs_hp_clamp_db(i, 100));

		CHECK(f <= hs_hp_vol_field(HS_HP_CEIL_DB), "field %#x for %d dB over the ceiling", f, i);
	}

	if (fails) {
		printf("%d failed\n", fails);
		return 1;
	}
	printf("headset-test: all passed\n");
	return 0;
}
