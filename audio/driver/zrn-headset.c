// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-headset.c: the 3.5 mm jack of the Galaxy S9, on the CS47L93 codec.
 * Included by snd-zrn-abox.c (one translation unit: it uses that file's codec
 * bus, codec_lock, stream_lock and capture path), kept apart so the jack's
 * code and the DSP's don't edit the same lines. audio/HEADSET.md has the
 * wiring and where every value comes from.
 *
 * What it does, with headset=1 (the default 0 leaves the codec as before):
 *
 *  - jack detection on the codec's own detect pins (JD1 and JD2 through the
 *    microphone-detect clamp, cirrus,jd-use-jd2 and clamp mode 8 as stock);
 *  - on insertion, one measurement of the sleeve through MICBIAS2A in ADC
 *    mode (stock's micd-software-compare): a microphone (1258 ohm or more, or
 *    open) is a 4-pole headset (TRRS), a short is 3-pole headphones (TRS);
 *  - with a headset, the button ladder in level mode with stock's ranges;
 *  - an input device "S9 Headset Jack": SW_JACK_PHYSICAL_INSERT,
 *    SW_HEADPHONE_INSERT, SW_MICROPHONE_INSERT, and KEY_PLAYPAUSE,
 *    KEY_VOICECOMMAND, KEY_VOLUMEUP, KEY_VOLUMEDOWN;
 *  - with hp_route=1, playback goes to the jack (HPOUT2, stock's
 *    route-headphone) instead of the speakers while headphones are in, at a
 *    level capped in the driver (zrn-headset-logic.h, HS_HP_CEIL_DB);
 *  - with headset_mic=1, recording takes the headset's microphone (IN1BL,
 *    stock's route-headset-mic) while a headset is in.
 *
 * Polled, not interrupt driven: the codec's interrupt line (gpa2-2) would
 * need its pin set to an external interrupt, which isn't approved yet. The
 * jack is read every headset_poll_ms on a deferrable timer (it never wakes an
 * idle CPU by itself), the buttons every headset_button_ms while a headset is
 * in. Over a SoC sleep the codec loses its settings (it comes back at reset,
 * seen 2026-10-30); the next poll notices and sets it up again.
 *
 * Accessory detection is independent of the DSP: none of it touches ABOX,
 * so the DSP may be off. Only the headphone route (hp_route) does, and only
 * while a stream plays.
 */
#include <linux/input.h>
#include <linux/mfd/madera/registers.h>
#include "zrn-headset-logic.h"

#define HS_OUT_VU	0x0200	/* OUT_VU, bit 9 of DAC Digital Volume nL/nR */

static int headset;
module_param(headset, int, 0444);
MODULE_PARM_DESC(headset, "3.5 mm jack: 0 off (no codec writes), 1 detection and buttons");
static unsigned int headset_poll_ms = 500;
module_param(headset_poll_ms, uint, 0644);
static unsigned int headset_button_ms = 40;
module_param(headset_button_ms, uint, 0644);
static bool hp_route;
module_param(hp_route, bool, 0444);
MODULE_PARM_DESC(hp_route, "play to the jack's HPOUT2 while headphones are in");
static bool headset_mic;
module_param(headset_mic, bool, 0444);
MODULE_PARM_DESC(headset_mic, "record from the headset's microphone while a headset is in");
static int hs_mic_gain = 12;	/* 0.5 dB steps over 0 dB: +6 dB */
module_param(hs_mic_gain, int, 0644);
static int hp_max_db = HS_HP_CEIL_DB;
module_param(hp_max_db, int, 0444);
MODULE_PARM_DESC(hp_max_db, "headphone ceiling in dB, never above " __stringify(HS_HP_CEIL_DB));
static unsigned int hs_unplug_quiet_ms = 1500;
module_param(hs_unplug_quiet_ms, uint, 0644);

static int hp_db = HS_HP_DEFAULT_DB;

struct hs_state {
	bool up;		/* accessory detection set up in the codec */
	bool present;		/* a plug in the jack */
	enum hs_kind kind;	/* what it is, once measured */
	int ohms;		/* the insertion reading, -1 none */
	int range;		/* button range held (1-based), 0 none */
	int pending, pending_n;	/* debouncing a change of range */
	unsigned int key;	/* key reported down */
	bool micvdd;		/* holding MICVDD (the charge pump) on */
	bool hp_on;		/* HPOUT2 playing */
	bool mic_routed;	/* AIF1TX takes IN1L */
	bool quiet;		/* speakers held off just after an unplug */
	unsigned int setups, inserts, presses, lost;
};

static struct hs_state hs = { .ohms = -1 };
static DEFINE_MUTEX(hs_lock);
static struct input_dev *hs_input;
static struct delayed_work hs_poll_work, hs_button_work, hs_quiet_work;
static bool hs_running;

/* ---- codec writes: every one listed in audio/HEADSET.md ------------------- */

/* codec_lock held */
static void hs_codec_setup(void)
{
	struct hs_level_regs lv;
	int i;

	hs_compute_levels(&lv);
	/* 32 kHz clock for the detect logic from MCLK2 (madera-core, always) */
	codec_update(MADERA_CLOCK_32K_1, MADERA_CLK_32K_ENA | MADERA_CLK_32K_SRC_MASK,
		MADERA_CLK_32K_ENA | 1);
	/* cirrus,gpsw = <3 0> */
	codec_update(MADERA_GP_SWITCH_1, MADERA_SW1_MODE_MASK, 3);
	/* micd-bias-start-time 1, micd-rate 7, bias MICBIAS2A (micd-configs bias
	 * 4), micd-dbtime 1, not enabled */
	codec_update(MADERA_MIC_DETECT_1_CONTROL_1,
		MADERA_MICD_BIAS_STARTTIME_MASK | MADERA_MICD_RATE_MASK |
		MADERA_MICD_BIAS_SRC_MASK | MADERA_MICD_DBTIME | MADERA_MICD_ENA,
		(1 << MADERA_MICD_BIAS_STARTTIME_SHIFT) | (7 << MADERA_MICD_RATE_SHIFT) |
		(4 << MADERA_MICD_BIAS_SRC_SHIFT) | MADERA_MICD_DBTIME);
	/* sense MICDET1, ground MICDET2 (micd-configs src 0, gnd 1), level mode */
	codec_update(MADERA_MIC_DETECT_1_CONTROL_0,
		MADERA_MICD1_ADC_MODE_MASK | MADERA_MICD1_SENSE_MASK | MADERA_MICD1_GND_MASK,
		1 << MADERA_MICD1_GND_SHIFT);
	codec_update(MADERA_HEADPHONE_DETECT_0, MADERA_HPD_GND_SEL_MASK, 1);
	/* HPOUT2's ground feedback: HPOUTFB2 (micd-configs hp_gnd 1) */
	codec_update(MADERA_OUTPUT_PATH_CONFIG_1 + 8, MADERA_HP1_GND_SEL_MASK, 1);
	/* the button levels: stock's micd-ranges */
	codec_update(MADERA_MIC_DETECT_1_CONTROL_2, MADERA_MICD_LVL_SEL_MASK, lv.lvl_sel);
	for (i = 0; i < 4; i++)
		codec_update(MADERA_MIC_DETECT_1_LEVEL_1 + i, 0x3f3f, lv.level[i]);
	/* micd-clamp-mode 8 (JD1 low and JD2 low: no jack), override still on */
	codec_update(MADERA_MICD_CLAMP_CONTROL, MADERA_MICD_CLAMP_MODE_MASK, 8);
	codec_update(MADERA_INTERRUPT_DEBOUNCE_7,
		MADERA_MICD_CLAMP_DB | MADERA_JD1_DB | MADERA_JD2_DB,
		MADERA_MICD_CLAMP_DB | MADERA_JD1_DB | MADERA_JD2_DB);
	/* last: it is also the mark that the setup is in place */
	codec_update(MADERA_JACK_DETECT_ANALOGUE, MADERA_JD1_ENA | MADERA_JD2_ENA,
		MADERA_JD1_ENA | MADERA_JD2_ENA);
}

static bool hs_codec_setup_intact(void)
{
	return (codec_read(MADERA_JACK_DETECT_ANALOGUE) & 3) == 3;
}

/* A plug in the jack: the clamp's raw status low (madera_jack_present with
 * jd-use-jd2). codec_lock held. */
static int hs_codec_present(void)
{
	u16 v = codec_read(MADERA_IRQ1_RAW_STATUS_7);

	if (v == 0xffff)
		return -EIO;
	return !(v & MADERA_MICD_CLAMP_RISE_STS1);
}

/* MICVDD 3.0 V from the charge pump, as codec_start (MICBIAS2 runs from it).
 * codec_stop leaves it on while we hold it. codec_lock held. */
static void hs_micvdd(bool on)
{
	if (on == hs.micvdd)
		return;
	hs.micvdd = on;
	if (on) {
		codec_update(CODEC_LDO2_CONTROL_1, 0x07e0, 0x24 << 5);
		codec_update(CODEC_MIC_CP_1, 0x0003, 0x0001);
		usleep_range(3000, 4000);
	} else if (!cap.path_on) {
		codec_update(CODEC_MIC_CP_1, 0x0003, 0x0002);	/* off, bypass: reset default */
	}
}

static bool hs_micvdd_held(void)
{
	return READ_ONCE(hs.micvdd);
}

/* MICBIAS2 2.8 V, external capacitor (stock DT MICBIAS2), and its 2A output
 * to the jack's microphone contact (micd-force-micbias). codec_lock held. */
static void hs_bias(bool on)
{
	if (on) {
		codec_update(MADERA_MIC_BIAS_CTRL_2, MADERA_MICB1_EXT_CAP | MADERA_MICB1_LVL_MASK |
			MADERA_MICB1_ENA, MADERA_MICB1_EXT_CAP | (13 << 5) | MADERA_MICB1_ENA);
		codec_update(MADERA_MIC_BIAS_CTRL_6, MADERA_MICB2A_ENA, MADERA_MICB2A_ENA);
	} else {
		codec_update(MADERA_MIC_BIAS_CTRL_6, MADERA_MICB2A_ENA, 0);
		codec_update(MADERA_MIC_BIAS_CTRL_2, MADERA_MICB1_ENA, 0);
	}
}

/* the detect circuit back to idle: clamp override on, as madera_micd_stop */
static void hs_micd_idle(void)
{
	codec_update(MADERA_MIC_DETECT_1_CONTROL_1, MADERA_MICD_ENA, 0);
	codec_update(MADERA_MIC_DETECT_1_CONTROL_0, MADERA_MICD1_ADC_MODE_MASK, 0);
	hs_bias(false);
	hs_micvdd(false);
	codec_update(MADERA_MICD_CLAMP_CONTROL, MADERA_MICD_CLAMP_OVD, MADERA_MICD_CLAMP_OVD);
}

/* One reading of the sleeve in ADC mode (madera_micd_mic_start, then
 * madera_micd_adc_read after stock's 300 ms micd-detect-debounce). Leaves the
 * bias on. codec_lock taken here: the sleep is outside it. */
static int hs_measure(void)
{
	u16 adc;

	mutex_lock(&codec_lock);
	codec_update(MADERA_MICD_CLAMP_CONTROL, MADERA_MICD_CLAMP_OVD, 0);
	hs_micvdd(true);
	hs_bias(true);
	codec_update(MADERA_MIC_DETECT_1_CONTROL_0, MADERA_MICD1_ADC_MODE_MASK,
		MADERA_MICD1_ADC_MODE_MASK);
	codec_update(MADERA_MIC_DETECT_1_CONTROL_1, MADERA_MICD_ENA, MADERA_MICD_ENA);
	mutex_unlock(&codec_lock);
	msleep(300);
	mutex_lock(&codec_lock);
	codec_update(MADERA_MIC_DETECT_1_CONTROL_1, MADERA_MICD_ENA, 0);	/* before ADCVAL */
	adc = codec_read(MADERA_MIC_DETECT_1_CONTROL_4);
	mutex_unlock(&codec_lock);
	pr_info("snd-zrn-abox: jack: MICDET ADC %04x\n", adc);
	return hs_adc_ohms(adc);
}

/* level mode, continuously, for the buttons (madera_micd_button_start) */
static void hs_buttons_on(void)
{
	mutex_lock(&codec_lock);
	codec_update(MADERA_MIC_DETECT_1_CONTROL_0, MADERA_MICD1_ADC_MODE_MASK, 0);
	codec_update(MADERA_MIC_DETECT_1_CONTROL_1, MADERA_MICD_ENA, MADERA_MICD_ENA);
	mutex_unlock(&codec_lock);
}

/* ---- the headset microphone (headset_mic=1) --------------------------------- */

/* codec_lock held; the capture path running (SYSCLK on) */
static void hs_mic_route_locked(bool on)
{
	if (on == hs.mic_routed)
		return;
	if (on) {
		/* IN1L from its B pins, differential, analogue (stock IN1L Mux B,
		 * cirrus,inmode IN1BL = 0) */
		codec_update(MADERA_IN1L_CONTROL, MADERA_IN1L_SRC_MASK | MADERA_IN1L_SRC_SE_MASK |
			MADERA_IN1_MODE_MASK, MADERA_IN1L_SRC_MASK);
		codec_update(MADERA_INPUT_ENABLES, MADERA_IN1L_ENA, MADERA_IN1L_ENA);
		usleep_range(1000, 3000);
		codec_update(MADERA_ADC_DIGITAL_VOLUME_1L, MADERA_IN1L_MUTE | MADERA_IN1L_DIG_VOL_MASK,
			0x80 + clamp(hs_mic_gain, 0, 48));
		codec_update(MADERA_ADC_DIGITAL_VOLUME_1L, MADERA_IN_VU, MADERA_IN_VU);
		codec_write(CODEC_AIF1TX1MIX_SRC1, 0x10);	/* IN1L */
		codec_write(CODEC_AIF1TX2MIX_SRC1, 0x10);
	} else {
		codec_write(CODEC_AIF1TX1MIX_SRC1, CODEC_SRC_IN3L);
		codec_write(CODEC_AIF1TX2MIX_SRC1, CODEC_SRC_IN3R);
		codec_update(MADERA_ADC_DIGITAL_VOLUME_1L, MADERA_IN1L_MUTE | MADERA_IN_VU,
			MADERA_IN1L_MUTE | MADERA_IN_VU);
		codec_update(MADERA_INPUT_ENABLES, MADERA_IN1L_ENA, 0);
	}
	hs.mic_routed = on;
	pr_info("snd-zrn-abox: jack: recording from the %s\n", on ? "headset microphone" : "phone's microphones");
}

static bool hs_mic_wanted(void)
{
	return headset == 1 && headset_mic && hs.present && hs.kind == HS_HEADSET;
}

/* called by cap_path_on after codec_start, and by cap_path_off before
 * codec_stop; stream_lock held */
static void hs_capture_started(void)
{
	if (!hs_mic_wanted())
		return;
	mutex_lock(&codec_lock);
	hs_mic_route_locked(true);
	mutex_unlock(&codec_lock);
}

static void hs_capture_stopping(void)
{
	if (!hs.mic_routed)
		return;
	mutex_lock(&codec_lock);
	hs_mic_route_locked(false);
	mutex_unlock(&codec_lock);
}

/* a button down mutes the microphone, as stock (madera_hs_mic_control) */
static void hs_mic_mute(bool mute)
{
	if (!hs.mic_routed)
		return;
	mutex_lock(&codec_lock);
	codec_update(MADERA_ADC_DIGITAL_VOLUME_1L, MADERA_IN1L_MUTE | MADERA_IN_VU,
		(mute ? MADERA_IN1L_MUTE : 0) | MADERA_IN_VU);
	mutex_unlock(&codec_lock);
}

/* ---- the headphone output (hp_route=1) --------------------------------------- */

static bool hs_hp_wanted(void)
{
	return headset == 1 && hp_route && hs.present && hs.kind != HS_NONE && !call_route;
}

/* amp_set asks: the speakers stay off while the jack plays, and just after an
 * unplug (until the player has had time to stop: hs_unplug_quiet_ms) */
static bool hs_speakers_held_off(void)
{
	return hs_hp_wanted() || READ_ONCE(hs.quiet);
}

static void hs_hp_volume_locked(bool mute)
{
	unsigned int f = hs_hp_vol_field(hs_hp_clamp_db(hp_db, hp_max_db));
	u16 m = mute ? MADERA_OUT2L_MUTE : 0;

	codec_update(MADERA_DAC_DIGITAL_VOLUME_2L, MADERA_OUT2L_MUTE | MADERA_OUT2L_VOL_MASK, m | f);
	codec_update(MADERA_DAC_DIGITAL_VOLUME_2R, HS_OUT_VU | MADERA_OUT2R_MUTE | MADERA_OUT2R_VOL_MASK,
		HS_OUT_VU | m | f);
}

/* stream_lock held, the DSP running */
static int hs_hp_on(void)
{
	int ret;

	if (hs.hp_on)
		return 0;
	ret = codec_power_up();
	if (!ret)
		ret = cap_path_on();	/* UAIF0's clocks, the codec's FLL and SYSCLK */
	if (ret) {
		pr_err("snd-zrn-abox: jack: no headphone path (%d)\n", ret);
		return ret;
	}
	amp_set(false);
	/* ABOX: SIFS0 (every stream's mix) also out of UAIF0 to the codec, stock
	 * route-sifs0-to-uaif0 */
	sfr_field(ABOX_ROUTE_CTRL0, 0, 0xf, 1);
	writel(readl(sfr + ABOX_UAIF0_CTRL0) | BIT(0), sfr + ABOX_UAIF0_CTRL0);
	mutex_lock(&codec_lock);
	/* volume up 8 ms/6 dB, down 1 ms/6 dB */
	codec_update(MADERA_OUTPUT_VOLUME_RAMP, MADERA_OUT_VD_RAMP_MASK | MADERA_OUT_VI_RAMP_MASK,
		(2 << MADERA_OUT_VD_RAMP_SHIFT) | 5);
	hs_hp_volume_locked(true);
	/* stock route-headphone: HPOUT2L <- AIF1RX1, HPOUT2R <- AIF1RX2 */
	codec_write(MADERA_OUT2LMIX_INPUT_1_SOURCE, 0x20);
	codec_write(MADERA_OUT2RMIX_INPUT_1_SOURCE, 0x21);
	codec_update(MADERA_AIF1_RX_ENABLES, MADERA_AIF1RX1_ENA | MADERA_AIF1RX2_ENA,
		MADERA_AIF1RX1_ENA | MADERA_AIF1RX2_ENA);
	codec_update(MADERA_OUTPUT_ENABLES_1, MADERA_OUT2L_ENA | MADERA_OUT2R_ENA,
		MADERA_OUT2L_ENA | MADERA_OUT2R_ENA);
	usleep_range(6000, 7000);	/* madera_out_ev: CS47L93 output up delay */
	hs_hp_volume_locked(false);	/* ramps up from silence to hp_db */
	mutex_unlock(&codec_lock);
	hs.hp_on = true;
	pr_info("snd-zrn-abox: jack: playing to the headphones at %d dB\n",
		hs_hp_clamp_db(hp_db, hp_max_db));
	return 0;
}

/* stream_lock held */
static void hs_hp_off(void)
{
	if (!hs.hp_on)
		return;
	mutex_lock(&codec_lock);
	hs_hp_volume_locked(true);
	mutex_unlock(&codec_lock);
	msleep(15);
	mutex_lock(&codec_lock);
	codec_update(MADERA_OUTPUT_ENABLES_1, MADERA_OUT2L_ENA | MADERA_OUT2R_ENA, 0);
	usleep_range(1000, 2000);
	codec_update(MADERA_AIF1_RX_ENABLES, MADERA_AIF1RX1_ENA | MADERA_AIF1RX2_ENA, 0);
	codec_write(MADERA_OUT2LMIX_INPUT_1_SOURCE, 0);
	codec_write(MADERA_OUT2RMIX_INPUT_1_SOURCE, 0);
	mutex_unlock(&codec_lock);
	if (booted && !suspending) {
		writel(readl(sfr + ABOX_UAIF0_CTRL0) & ~BIT(0), sfr + ABOX_UAIF0_CTRL0);
		sfr_field(ABOX_ROUTE_CTRL0, 0, 0xf, 0);
	}
	hs.hp_on = false;
	if (!cap.opened)
		cap_path_off();
	pr_info("snd-zrn-abox: jack: headphones off\n");
}

static bool hs_hp_active(void)
{
	return READ_ONCE(hs.hp_on);
}

/* abox_pcm_prepare, stream_lock held, after amp_set */
static void hs_stream_prepare(void)
{
	if (hs_hp_wanted() && booted && !suspending)
		hs_hp_on();
}

/* abox_pcm_close with no stream left, stream_lock held */
static void hs_streams_idle(void)
{
	hs_hp_off();
}

static void hs_quiet_fn(struct work_struct *w)
{
	mutex_lock(&stream_lock);
	WRITE_ONCE(hs.quiet, false);
	if (booted && !suspending && !call_route && any_active() && !hs_hp_wanted())
		amp_set(true);
	mutex_unlock(&stream_lock);
}

/* the jack changed while streams may be playing: move the sound. hs_lock held. */
static void hs_route_changed(void)
{
	mutex_lock(&stream_lock);
	if (hs_hp_wanted()) {
		if (booted && !suspending && any_active())
			hs_hp_on();
	} else if (hs.hp_on) {
		/* unplugged while playing: neither the jack nor the speakers until
		 * the player stops or hs_unplug_quiet_ms passes */
		WRITE_ONCE(hs.quiet, true);
		hs_hp_off();
		mod_delayed_work(system_wq, &hs_quiet_work, msecs_to_jiffies(hs_unplug_quiet_ms));
	}
	if (cap.path_on) {
		mutex_lock(&codec_lock);
		hs_mic_route_locked(hs_mic_wanted());
		mutex_unlock(&codec_lock);
	}
	mutex_unlock(&stream_lock);
}

/* ---- reporting ---------------------------------------------------------------- */

static void hs_report_key(unsigned int key)
{
	if (key == hs.key)
		return;
	if (hs.key) {
		input_report_key(hs_input, hs.key, 0);
		input_sync(hs_input);
	}
	hs.key = key;
	if (key) {
		input_report_key(hs_input, key, 1);
		input_sync(hs_input);
		hs.presses++;
	}
	hs_mic_mute(!!key);
}

static void hs_report_jack(void)
{
	input_report_switch(hs_input, SW_JACK_PHYSICAL_INSERT, hs.present);
	input_report_switch(hs_input, SW_HEADPHONE_INSERT, hs.present && hs.kind != HS_NONE);
	input_report_switch(hs_input, SW_MICROPHONE_INSERT, hs.present && hs.kind == HS_HEADSET);
	input_sync(hs_input);
}

/* ---- the state machine, polled -------------------------------------------------- */

/* hs_lock held */
static int hs_ensure_setup(void)
{
	bool intact;
	int ret;

	if (hs.up) {
		mutex_lock(&codec_lock);
		intact = hs_codec_setup_intact();
		mutex_unlock(&codec_lock);
		if (intact)
			return 0;
		/* reset under us (a SoC sleep): everything the codec held is gone.
		 * A plug still in is measured again as if just inserted (the
		 * switches don't change, so nothing is reported for it). */
		hs.up = false;
		hs.micvdd = false;
		hs.hp_on = false;
		hs.mic_routed = false;
		hs.lost++;
		hs_report_key(0);
		hs.present = false;
		hs.range = hs.pending = hs.pending_n = 0;
		pr_info("snd-zrn-abox: jack: the codec was reset: setting detection up again\n");
	}
	mutex_lock(&stream_lock);
	ret = codec_power_up();
	mutex_unlock(&stream_lock);
	if (ret)
		return ret;
	mutex_lock(&codec_lock);
	hs_codec_setup();
	mutex_unlock(&codec_lock);
	hs.up = true;
	hs.setups++;
	return 0;
}

static void hs_removed(void)
{
	hs_report_key(0);
	hs.present = false;
	hs.kind = HS_NONE;
	hs.ohms = -1;
	hs.range = hs.pending = hs.pending_n = 0;
	cancel_delayed_work(&hs_button_work);
	mutex_lock(&codec_lock);
	hs_micd_idle();
	mutex_unlock(&codec_lock);
	hs_report_jack();
	hs_route_changed();
	pr_info("snd-zrn-abox: jack: unplugged\n");
}

static void hs_inserted(void)
{
	int ohms, present;

	hs.present = true;
	hs.kind = HS_NONE;
	hs.inserts++;
	/* physically in; what it is follows */
	input_report_switch(hs_input, SW_JACK_PHYSICAL_INSERT, 1);
	input_sync(hs_input);
	msleep(30);			/* cirrus,init-delay-ms */
	ohms = hs_measure();
	mutex_lock(&codec_lock);
	present = hs_codec_present();
	mutex_unlock(&codec_lock);
	if (present <= 0) {		/* out again already */
		hs_removed();
		return;
	}
	hs.ohms = ohms;
	hs.kind = hs_classify(ohms);
	if (hs.kind == HS_HEADSET) {
		hs_buttons_on();	/* the bias stays on: it is the microphone's supply */
		mod_delayed_work(system_wq, &hs_button_work, msecs_to_jiffies(headset_button_ms));
	} else {
		mutex_lock(&codec_lock);
		hs_micd_idle();
		mutex_unlock(&codec_lock);
	}
	pr_info("snd-zrn-abox: jack: %s (sleeve %d ohm)\n",
		hs.kind == HS_HEADSET ? "headset, 4-pole (TRRS)" : "headphones, 3-pole (TRS)",
		ohms == HS_OPEN_OHM ? -1 : ohms);
	hs_report_jack();
	hs_route_changed();
}

static void hs_poll_fn(struct work_struct *w)
{
	int present;

	mutex_lock(&hs_lock);
	if (!hs_running)
		goto out;
	if (hs_ensure_setup())
		goto again;
	mutex_lock(&codec_lock);
	present = hs_codec_present();
	mutex_unlock(&codec_lock);
	if (present < 0)
		goto again;
	if (present && !hs.present)
		hs_inserted();
	else if (!present && hs.present)
		hs_removed();
again:
	queue_delayed_work(system_freezable_wq, &hs_poll_work,
		msecs_to_jiffies(max(headset_poll_ms, 100U)));
out:
	mutex_unlock(&hs_lock);
}

static void hs_button_fn(struct work_struct *w)
{
	int range;
	u16 v;

	mutex_lock(&hs_lock);
	if (!hs_running || !hs.present || hs.kind != HS_HEADSET) {
		mutex_unlock(&hs_lock);
		return;
	}
	mutex_lock(&codec_lock);
	v = codec_read(MADERA_MIC_DETECT_1_CONTROL_3);
	mutex_unlock(&codec_lock);
	range = v == 0xffff ? -1 : hs_button_range(v);
	if (range >= 0) {
		/* two equal readings in a row before a change counts */
		if (range != hs.pending) {
			hs.pending = range;
			hs.pending_n = 1;
		} else if (++hs.pending_n >= 2 && range != hs.range) {
			hs.range = range;
			hs_report_key(hs_range_key(range));
		}
	}
	queue_delayed_work(system_freezable_wq, &hs_button_work,
		msecs_to_jiffies(max(headset_button_ms, 10U)));
	mutex_unlock(&hs_lock);
}

/* what the jack is doing, for the bench: cat /sys/module/snd_zrn_abox/parameters/headset_state */
static int hs_state_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%s up=%d present=%d kind=%s ohms=%d key=%u hp_on=%d mic=%d quiet=%d micvdd=%d setups=%u inserts=%u presses=%u lost=%u hp_db=%d\n",
		headset != 1 ? "off" : "on", hs.up, hs.present,
		hs.kind == HS_HEADSET ? "headset" : hs.kind == HS_HEADPHONES ? "headphones" : "none",
		hs.ohms == HS_OPEN_OHM ? -1 : hs.ohms, hs.key, hs.hp_on, hs.mic_routed, hs.quiet,
		hs.micvdd, hs.setups, hs.inserts, hs.presses, hs.lost,
		hs_hp_clamp_db(hp_db, hp_max_db));
}
static const struct kernel_param_ops hs_state_ops = { .get = hs_state_get };
module_param_cb(headset_state, &hs_state_ops, NULL, 0444);

/* the headphone level in dB, clamped to the ceiling; applied at once if playing */
static int hp_db_set(const char *val, const struct kernel_param *kp)
{
	int db, ret = kstrtoint(val, 0, &db);

	if (ret)
		return ret;
	mutex_lock(&stream_lock);
	hp_db = hs_hp_clamp_db(db, hp_max_db);
	if (hs.hp_on) {
		mutex_lock(&codec_lock);
		hs_hp_volume_locked(false);
		mutex_unlock(&codec_lock);
	}
	mutex_unlock(&stream_lock);
	return 0;
}
static const struct kernel_param_ops hp_db_ops = { .set = hp_db_set, .get = param_get_int };
module_param_cb(hp_db, &hp_db_ops, &hp_db, 0644);
MODULE_PARM_DESC(hp_db, "headphone level in dB (" __stringify(HS_HP_FLOOR_DB) " to the ceiling)");

static int hs_pm_event(struct notifier_block *nb, unsigned long event, void *unused)
{
	switch (event) {
	case PM_SUSPEND_PREPARE:
		mutex_lock(&hs_lock);
		hs_running = false;
		mutex_unlock(&hs_lock);
		cancel_delayed_work_sync(&hs_poll_work);
		cancel_delayed_work_sync(&hs_button_work);
		mutex_lock(&stream_lock);
		hs_hp_off();	/* before the DSP goes down (this notifier runs first) */
		mutex_unlock(&stream_lock);
		break;
	case PM_POST_SUSPEND:
		mutex_lock(&hs_lock);
		hs_running = true;
		mutex_unlock(&hs_lock);
		mod_delayed_work(system_freezable_wq, &hs_poll_work, 0);
		break;
	}
	return NOTIFY_DONE;
}
static struct notifier_block hs_pm = { .notifier_call = hs_pm_event, .priority = 1 };

/* abox_init, before the card is registered */
static int hs_init(void)
{
	int ret, i;

	INIT_DEFERRABLE_WORK(&hs_poll_work, hs_poll_fn);
	INIT_DELAYED_WORK(&hs_button_work, hs_button_fn);
	INIT_DELAYED_WORK(&hs_quiet_work, hs_quiet_fn);
	hp_db = hs_hp_clamp_db(hp_db, hp_max_db);
	if (headset != 1)
		return 0;
	if (!capture) {
		pr_warn("snd-zrn-abox: jack: needs capture=1 (the codec's bus): off\n");
		headset = 0;
		return 0;
	}
	hs_input = input_allocate_device();
	if (!hs_input)
		return -ENOMEM;
	hs_input->name = "S9 Headset Jack";
	hs_input->phys = "snd-zrn-abox/cs47l93/jack";
	hs_input->id.bustype = BUS_HOST;
	input_set_capability(hs_input, EV_SW, SW_JACK_PHYSICAL_INSERT);
	input_set_capability(hs_input, EV_SW, SW_HEADPHONE_INSERT);
	input_set_capability(hs_input, EV_SW, SW_MICROPHONE_INSERT);
	for (i = 0; i < HS_NUM_RANGES; i++)
		if (hs_ranges[i].key)
			input_set_capability(hs_input, EV_KEY, hs_ranges[i].key);
	ret = input_register_device(hs_input);
	if (ret) {
		input_free_device(hs_input);
		hs_input = NULL;
		return ret;
	}
	ret = register_pm_notifier(&hs_pm);
	if (ret) {
		input_unregister_device(hs_input);
		hs_input = NULL;
		return ret;
	}
	hs_running = true;
	queue_delayed_work(system_freezable_wq, &hs_poll_work, 0);
	pr_info("snd-zrn-abox: jack: detection on (polled every %u ms)%s%s\n", headset_poll_ms,
		hp_route ? ", headphone output" : "", headset_mic ? ", headset microphone" : "");
	return 0;
}

/* teardown, before the codec goes back into reset */
static void hs_exit(void)
{
	if (!hs_input)
		return;
	unregister_pm_notifier(&hs_pm);
	mutex_lock(&hs_lock);
	hs_running = false;
	mutex_unlock(&hs_lock);
	cancel_delayed_work_sync(&hs_poll_work);
	cancel_delayed_work_sync(&hs_button_work);
	cancel_delayed_work_sync(&hs_quiet_work);
	mutex_lock(&stream_lock);
	if (started)
		hs_hp_off();
	hs.quiet = false;
	mutex_unlock(&stream_lock);
	mutex_lock(&hs_lock);
	if (hs.present)
		hs_report_key(0);
	if (hs.up && codec_rst) {
		mutex_lock(&codec_lock);
		hs_micd_idle();	/* JD stays on: teardown puts the codec in reset next */
		mutex_unlock(&codec_lock);
	}
	hs.up = false;
	mutex_unlock(&hs_lock);
	input_unregister_device(hs_input);
	hs_input = NULL;
}
