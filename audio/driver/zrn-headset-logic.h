/* SPDX-License-Identifier: GPL-2.0 */
/*
 * zrn-headset-logic.h: the arithmetic of the Galaxy S9's 3.5 mm jack, with
 * no hardware access, shared by the driver (zrn-headset.c) and the host test
 * (audio/tests/headset-test.c). The includer defines the KEY_* codes
 * (<linux/input.h> in the kernel, <linux/input-event-codes.h> on the host)
 * and the fixed-width types.
 *
 * Every number here is the stock phone's: the codec node's cirrus,accdet
 * child in the stock device tree (stock/BOOT.img, Star rev26; the same in
 * rev16 to rev26) as Samsung's extcon-madera.c reads it, and that driver's
 * tables (third_party/vendor-kernel/drivers/extcon/extcon-madera.c).
 */
#ifndef ZRN_HEADSET_LOGIC_H
#define ZRN_HEADSET_LOGIC_H

/* Ohms for each MICDET level and ADC value (extcon-madera.c madera_micd_levels).
 * Index 64 is "open": no measurable load. */
static const int hs_micd_levels[] = {
	3, 6, 8, 11, 13, 16, 18, 21, 23, 26, 28, 31, 34, 36, 39, 41, 44, 46,
	49, 52, 54, 57, 60, 62, 65, 67, 70, 73, 75, 78, 81, 83, 89, 94, 100,
	105, 111, 116, 122, 127, 139, 150, 161, 173, 186, 196, 209, 220, 245,
	270, 295, 321, 348, 375, 402, 430, 489, 550, 614, 681, 752, 903, 1071,
	1257, 30000,
};
#define HS_NUM_BUTTON_LEVELS	64
#define HS_MAX_RANGES		8

/* At or above this a ring-to-sleeve load is a microphone (TRRS); below, the
 * sleeve is grounded with the ring: plain headphones (TRS). Open circuit
 * counts as a microphone, as stock (cirrus,micd-open-circuit-declare). */
#define HS_MIC_MIN_OHM		1258
#define HS_MIC_MAX_OHM		30000
#define HS_OPEN_OHM		0x7fffffff

/*
 * The buttons: stock cirrus,micd-ranges = <116 226 173 582 321 115 752 114
 * 1257 217>, the upper bound in ohms of each range and its key. Stock's keys
 * are KEY_MEDIA, KEY_VOICECOMMAND, KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_SEARCH.
 * Here KEY_MEDIA is reported as KEY_PLAYPAUSE (what desktop players and
 * playerctl expect), and the fifth range (753..1257 ohm, which no headset
 * standard uses) is measured as stock does but reports nothing. The register
 * values stay exactly stock's.
 * Android's headset spec: play/pause 0 ohm, voice 135, volume up 240, volume
 * down 470.
 */
struct hs_range {
	int max;		/* ohms */
	unsigned int key;	/* 0: none */
};

static const struct hs_range hs_ranges[] = {
	{ 116,  KEY_PLAYPAUSE },
	{ 173,  KEY_VOICECOMMAND },
	{ 321,  KEY_VOLUMEUP },
	{ 752,  KEY_VOLUMEDOWN },
	{ 1257, 0 },
};
#define HS_NUM_RANGES	(int)(sizeof(hs_ranges) / sizeof(hs_ranges[0]))

/* Register values the ranges give (madera_extcon_add_micd_levels and
 * madera_micd_set_level): Mic Detect 1 Level 1..4 (0x2a6..0x2a9) and the
 * level-select byte of Mic Detect 1 Control 2 (0x2a4). Returns -1 if a range
 * is beyond the table. */
struct hs_level_regs {
	unsigned int level[4];	/* 0x2a6, 0x2a7, 0x2a8, 0x2a9 */
	unsigned int lvl_sel;	/* 0x2a4 bits 7:0 */
};

static inline int hs_level_index(int max)
{
	int j;

	for (j = 0; j < HS_NUM_BUTTON_LEVELS; j++)
		if (hs_micd_levels[j] >= max)
			return j;
	return -1;
}

static inline int hs_compute_levels(struct hs_level_regs *r)
{
	int i, j;

	/* the codec's reset values, which every slot is then set over */
	r->level[0] = 0x3d3d;
	r->level[1] = 0x3d3d;
	r->level[2] = 0x333d;
	r->level[3] = 0x202d;
	r->lvl_sel = 0x81;
	for (i = 0; i < HS_MAX_RANGES; i++) {
		int reg = 3 - i / 2;	/* LEVEL_4 - i / 2 */

		if (i < HS_NUM_RANGES) {
			j = hs_level_index(hs_ranges[i].max);
			if (j < 0)
				return -1;
			r->lvl_sel |= 1u << i;
		} else {
			j = 0x3f;
		}
		if (i % 2 == 0)
			r->level[reg] = (r->level[reg] & ~0x3f00u) | ((unsigned int)j << 8);
		else
			r->level[reg] = (r->level[reg] & ~0x003fu) | (unsigned int)j;
	}
	return 0;
}

/* Mic Detect 1 Control 4 (0x2ab) ADCVAL, in ADC mode, to ohms
 * (madera_micd_adc_read). */
static inline int hs_adc_ohms(unsigned int reg)
{
	unsigned int v = reg & 0x7f;

	if (v < sizeof(hs_micd_levels) / sizeof(hs_micd_levels[0]))
		return hs_micd_levels[v];
	return HS_OPEN_OHM;
}

enum hs_kind { HS_NONE = 0, HS_HEADPHONES = 1, HS_HEADSET = 2 };

/* What was plugged in, from the ADC reading at insertion
 * (madera_micd_mic_reading with one micd-config: no polarity flip). */
static inline enum hs_kind hs_classify(int ohms)
{
	if (ohms < 0)
		return HS_HEADPHONES;	/* no reading: sound, but no microphone */
	if (ohms >= HS_MIC_MIN_OHM)
		return HS_HEADSET;	/* a microphone, or open (still a 4-pole plug) */
	return HS_HEADPHONES;
}

/* Mic Detect 1 Control 3 (0x2a5) in level mode, to a button.
 * Returns -1 while the reading isn't valid, 0 for no button, else the range
 * index + 1 (look up hs_ranges[ret - 1].key). Bits as madera_micd_read:
 * 0 MICD_STS, 1 MICD_VALID, 10:2 MICD_LVL_0..8. */
#define HS_MICD_STS		0x0001
#define HS_MICD_VALID		0x0002
#define HS_MICD_LVL_MASK	0x07fc
#define HS_MICD_LVL_0_TO_7	0x03fc

static inline int hs_button_range(unsigned int reg)
{
	unsigned int lvl;
	int i;

	if (!(reg & HS_MICD_VALID))
		return -1;
	if (!(reg & HS_MICD_STS))
		return 0;
	if (!(reg & HS_MICD_LVL_0_TO_7))
		return 0;	/* above every button: the microphone itself */
	lvl = (reg & HS_MICD_LVL_MASK) >> 2;
	for (i = 0; i < 9; i++)
		if (lvl & (1u << i))
			break;
	if (i >= HS_NUM_RANGES)
		return 0;
	return i + 1;
}

static inline unsigned int hs_range_key(int range)
{
	if (range <= 0 || range > HS_NUM_RANGES)
		return 0;
	return hs_ranges[range - 1].key;
}

/*
 * Headphone level, hearing safety. The codec's OUT2 digital volume is
 * 0.5 dB a step with 0x80 = 0 dB (DAC Digital Volume 2L/2R, 0x419/0x41d).
 * HS_HP_CEIL_DB is the most the driver ever sets, whatever it is asked;
 * hp_max_db (a module parameter) can only lower it. PipeWire's volume comes
 * on top of this, never above it.
 */
#define HS_HP_CEIL_DB		(-20)
#define HS_HP_FLOOR_DB		(-64)
#define HS_HP_DEFAULT_DB	(-40)

static inline int hs_hp_clamp_db(int db, int max_db)
{
	if (max_db > HS_HP_CEIL_DB)
		max_db = HS_HP_CEIL_DB;
	if (db > max_db)
		db = max_db;
	if (db < HS_HP_FLOOR_DB)
		db = HS_HP_FLOOR_DB;
	return db;
}

/* the 8-bit OUT2x_VOL field for a level in dB (already clamped) */
static inline unsigned int hs_hp_vol_field(int db)
{
	return (unsigned int)(0x80 + 2 * db) & 0xff;
}

#endif
