#!/usr/bin/env python3
"""The panel's own low-power mode and analogue clock: the always-on display
offloaded to the S6E3HA8 (ROADMAP 8.45, power/aod/PANEL-AOD.md).

  patch-decon-lpm.py <kernel tree>

Applied after patch-decon-aod.py (it uses its `kept` state). Adds, on the
DECON device next to panel_cmd:

  aod_lpm     read/write 0-4. 1-4: Samsung's HLPM entry at 2/10/30/60 nit
              (star_a3_s0_alpm_enter_cmdtbl with the HLPM row of
              star_a3_s0_lpm_nit_table); 0: its exit (the live-clock exit
              when the clock was set up, then star_a3_s0_alpm_exit_cmdtbl)
              and the whole brightness set again. Brightness writes are
              held off while the panel is in LPM: they would undo it.
  aod_sprite  write-only, 230400 bytes: the analogue clock's hands
              (480 x 120, RGBA, three 40-row bands: hour, minute, second),
              uploaded to the panel's side RAM at the next "analog".
  aod_clock   write-only, in LPM only:
                "analog X Y HH MM SS [24]"  the live-clock entry
                    (s6e3ha8_aod_enter_aod_cmdtbl) once, the sprite if the
                    side RAM lost it, the time (set_time_cmdtbl), then the
                    clock's place and switch (analog_ctrl_cmdtbl)
                "time HH MM SS [24]"        the time again (a wake's resync)
                "off"                       the clock off (77 00 00)

Every byte sent is one of Samsung's own sequences from the G960F kernel
(exynos-linux-stable/starlte, tw10-android-4.9-q), listed with their source
lines in power/aod/PANEL-AOD.md; nothing here writes F1, C0 or C1, and no
other command can be sent through these files. Both keys (F0, FC) are closed
at the end of every sequence, whatever happened in it. Each sequence goes
between two frames with the trigger held off; with the pipeline parked
(aod_keep) the trigger stays off afterwards.

Frames and the live clock's AOD timing (76 ... 85 A0 0B 90) do not mix
with this DECON: sent in normal mode on 2026-11-12 (the burn-in trial) the
flips stopped latching about two minutes later and only a power-off brought
the display back. So the clock is only set up with the pipeline parked
(aod_keep), and a pipeline enable while it is set up sends the live clock's
exit (76 09 0F 00, timer and blink off) before any frame goes; the shell
sets the clock up again after parking.

A panel sleep (the ordinary screen-off) ends all of it: sleep-out reloads
the panel's registers and the side RAM is lost, so the state is cleared in
s9p_panel_down.
"""
import sys
from pathlib import Path

path = Path(sys.argv[1]) / "drivers/gpu/drm/s9p/s9p-decon.c"
s = path.read_text()
if "zboot: panel lpm" in s:
    print("patch-decon-lpm: already applied")
    sys.exit(0)
if "zboot: aod keep" not in s:
    sys.exit("patch-decon-lpm: apply patch-decon-aod.py first")

CODE = r'''/* ------------------------------------------- zboot: panel lpm (AOD) */

#define S9P_SPRITE_SIZE		(480 * 120 * 4)
#define S9P_SRAM_CHUNK		496	/* panel_dsi_write_mem: 511, 16-aligned */

/* B0 n, the offset of the next write's parameters: a DCS short write with
 * one parameter, as the vendor's mipi_write sends its gpara */
static void s9p_lpm_gpara(struct s9p_decon *priv, u8 ofs)
{
	u32 v;

	writel_relaxed(DSIM_INTSRC_PH_FIFO_EMPTY, priv->dsim + DSIM_INTSRC);
	writel_relaxed(DSIM_PKTHDR_ID(MIPI_DCS_SHORT_WRITE_PARAM) |
		       DSIM_PKTHDR_DATA0(0xB0) | DSIM_PKTHDR_DATA1(ofs),
		       priv->dsim + DSIM_PKTHDR);
	if (readl_poll_timeout_atomic(priv->dsim + DSIM_INTSRC, v,
				      v & DSIM_INTSRC_PH_FIFO_EMPTY, 10, 20000))
		dev_warn_once(priv->drm.dev, "panel did not take B0 %02x\n", ofs);
}

/* one packet, then the lanes idle again before the next */
static void s9p_lpm_send(struct s9p_decon *priv, const u8 *data, size_t len)
{
	u32 v;

	s9p_dsim_write(priv, data, len);
	readl_poll_timeout_atomic(priv->dsim + DSIM_DPHY_STATUS, v,
				  (v & DSIM_DPHY_STOP_STATE_DAT) == DSIM_DPHY_STOP_STATE_DAT,
				  10, 5000);
}

#define LPM(...) ({							\
	static const u8 _b[] = { __VA_ARGS__ };				\
	s9p_lpm_send(priv, _b, sizeof(_b));				\
})
#define KEY2_ON()	LPM(0xF0, 0x5A, 0x5A)
#define KEY2_OFF()	LPM(0xF0, 0xA5, 0xA5)
#define KEY3_ON()	LPM(0xFC, 0x5A, 0x5A)
#define KEY3_OFF()	LPM(0xFC, 0xA5, 0xA5)

/* between two frames: the trigger off and the last frame gone. False when a
 * frame would not finish (nothing was sent). Caller holds panel_lock. */
static bool s9p_lpm_begin(struct s9p_decon *priv)
{
	u32 v;

	if (!priv->dsim || !s9p_panel_dcs || !(priv->enabled || priv->kept))
		return false;
	s9p_hw_trigger_mask(priv);
	s9p_wait_shadow_idle(priv);
	if (readl_poll_timeout(priv->decon + DECON_GLOBAL_CONTROL, v,
			       v & DECON_GLOBAL_IDLE_STATUS, 20, 40000) ||
	    readl_poll_timeout_atomic(priv->dsim + DSIM_DPHY_STATUS, v,
				      (v & DSIM_DPHY_STOP_STATE_DAT) == DSIM_DPHY_STOP_STATE_DAT,
				      10, 5000)) {
		if (priv->enabled)
			s9p_hw_trigger_unmask(priv);
		return false;
	}
	return true;
}

/* both keys closed on every way out; frames again unless parked */
static void s9p_lpm_end(struct s9p_decon *priv)
{
	KEY2_OFF();
	KEY3_OFF();
	if (priv->enabled)
		s9p_hw_trigger_unmask(priv);
}

/* star_a3_s0_lpm_nit_table[HLPM_MODE] and lpm_mode_table: 2, 10, 30, 60 nit */
static const u8 s9p_hlpm_nit[4][7] = {
	{ 0xBB, 0x05, 0x0C, 0xBB, 0x4F, 0x4F, 0x84 },
	{ 0xBB, 0x05, 0x0C, 0x9B, 0xEE, 0x4F, 0x84 },
	{ 0xBB, 0x05, 0x0C, 0x6B, 0x3F, 0x4F, 0x84 },
	{ 0xBB, 0x05, 0x0C, 0x0B, 0x10, 0x4F, 0x04 },
};
static const u8 s9p_hlpm_mode[4][2] = {
	{ 0x53, 0x03 }, { 0x53, 0x02 }, { 0x53, 0x02 }, { 0x53, 0x02 },
};

/* star_a3_s0_alpm_enter_cmdtbl, HLPM row `row` (0-3). Also a change of level
 * while in LPM, as stock's panel_update_doze sends the whole entry again. */
static void s9p_lpm_enter(struct s9p_decon *priv, int row)
{
	/* __delay_normal_alpm: 124 ms after the panel came on, at least */
	unsigned long ready = priv->bl_on_at + msecs_to_jiffies(124);

	if (time_before(jiffies, ready))
		msleep(jiffies_to_msecs(ready - jiffies) + 1);
	KEY2_ON();
	LPM(0xB1, 0x0B, 0x74);			/* lpm_aor */
	LPM(0xF7, 0x03);			/* gamma_update_enable */
	KEY2_OFF();
	usleep_range(16700, 17000);		/* wait_1_frame_in_60hz */
	KEY2_ON();
	s9p_lpm_send(priv, s9p_hlpm_nit[row], 7);	/* lpm_nit */
	KEY3_ON();
	s9p_lpm_gpara(priv, 0x2B);
	LPM(0xFD, 0x03);			/* avs_on, offset 0x2B */
	KEY3_OFF();
	s9p_lpm_send(priv, s9p_hlpm_mode[row], 2);	/* lpm_mode */
	LPM(0xF4, 0xEB, 0x23, 0x0B, 0x17, 0x8A);	/* avc2_off */
	LPM(0xF7, 0x03);			/* gamma_update_enable */
	KEY2_OFF();
}

/* s6e3ha8_aod_exit_aod_cmdtbl (the live clock's exit), then
 * star_a3_s0_alpm_exit_cmdtbl with the HLPM lpm_off value */
static void s9p_aod_leave(struct s9p_decon *priv)
{
	if (priv->aod_entered) {
		KEY2_ON();
		LPM(0x76, 0x09, 0x0F, 0x00);	/* normal_setting */
		LPM(0x77, 0x00, 0x00);		/* timer_off_ctrl */
		LPM(0x79, 0x00, 0x00);		/* digital_blink_off */
		KEY2_OFF();
	}
	priv->aod_entered = false;
	priv->clock_on = false;
}

static void s9p_lpm_exit(struct s9p_decon *priv)
{
	s9p_aod_leave(priv);
	KEY2_ON();
	LPM(0xBB, 0x01);			/* lpm_off_nit (HLPM) */
	LPM(0xF4, 0xEB, 0x23, 0x0B, 0x17, 0xCA);	/* avc2_on */
	LPM(0x53, 0x00);			/* exit_alpm */
	KEY2_OFF();
	priv->aod_entered = false;
	priv->clock_on = false;
}

/* s6e3ha8_aod_self_mask_dis_cmdtbl and s6e3ha8_aod_enter_aod_cmdtbl, as
 * aod_enter_to_lpm sends them after the HLPM entry */
static void s9p_aod_enter(struct s9p_decon *priv)
{
	KEY2_ON();
	LPM(0x7A, 0x00);			/* self mask off (off since boot) */
	KEY2_OFF();
	KEY3_ON();
	s9p_lpm_gpara(priv, 0x15);
	LPM(0xFE, 0x00);			/* move_setting, offset 0x15 */
	LPM(0x76, 0x09, 0x0F, 0x85, 0xA0, 0x0B, 0x90);	/* aod_setting */
	KEY3_OFF();
	priv->aod_entered = true;
}

/* s6e3ha8_aod_analog_img_cmdtbl: init side RAM, path "analog", the image in
 * 4C/5C pieces as panel_dsi_write_mem cuts it */
static void s9p_aod_sprite_upload(struct s9p_decon *priv)
{
	u8 *buf = kmalloc(S9P_SRAM_CHUNK + 1, GFP_KERNEL);
	size_t done = 0, n;

	if (!buf)
		return;
	KEY2_ON();
	LPM(0x77, 0x00, 0x00);			/* init_side_ram */
	LPM(0x75, 0x01);			/* analog_sd_path */
	while (done < S9P_SPRITE_SIZE) {
		n = min_t(size_t, S9P_SPRITE_SIZE - done, S9P_SRAM_CHUNK);
		buf[0] = done ? 0x5C : 0x4C;	/* side RAM start / continue */
		memcpy(buf + 1, priv->sprite + done, n);
		s9p_lpm_send(priv, buf, n + 1);
		done += n;
	}
	KEY2_OFF();
	kfree(buf);
	priv->sram_ok = true;
}

/* s6e3ha8_aod_set_time_cmdtbl, filled as copy_set_time_ctrl fills it for an
 * analogue clock at the 1 s interval (rate 0x1E, step 0x13; the first after
 * the entry without bit 4) */
static void s9p_aod_set_time(struct s9p_decon *priv, int hh, int mm, int ss, bool h24)
{
	u8 t[16] = { 0x77, 0x00, 0x07, 0x00, hh, mm, ss, 0x00, 0x1E, 0x13 };

	if (h24)
		t[2] |= 0x20;			/* SC_24H_EN */
	if (priv->clock_first) {
		t[9] &= ~0x10;
		priv->clock_first = false;
	}
	KEY2_ON();
	s9p_lpm_send(priv, t, sizeof(t));
	LPM(0x78, 0x01);			/* update_time_ctrl */
	KEY2_OFF();
}

/* s6e3ha8_aod_analog_ctrl_cmdtbl: the place (offset 9) and the switch */
static void s9p_aod_analog_ctrl(struct s9p_decon *priv, bool h24)
{
	u8 pos[13] = { 0x77, priv->clk_x >> 8, priv->clk_x & 0xff,
		       priv->clk_y >> 8, priv->clk_y & 0xff, 0x00 /* ALG_ROTATE_0 */,
		       0x00, 0x78, 0x13, 0x78, 0x13, 0x78, 0x13 };
	u8 ctl[4] = { 0x77, 0x00, 0x07 | (h24 ? 0x20 : 0), 0x00 };

	KEY2_ON();
	s9p_lpm_gpara(priv, 0x09);
	s9p_lpm_send(priv, pos, sizeof(pos));
	s9p_lpm_send(priv, ctl, sizeof(ctl));
	KEY2_OFF();
}

static ssize_t aod_lpm_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct s9p_decon *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d%s%s\n", priv->lpm, priv->clock_on ? " clock" : "",
			  priv->kept ? " kept" : "");
}

static ssize_t aod_lpm_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct s9p_decon *priv = dev_get_drvdata(dev);
	unsigned int want;
	int ret = count;

	if (kstrtouint(buf, 0, &want) || want > 4)
		return -EINVAL;
	mutex_lock(&priv->panel_lock);
	if (want == priv->lpm) {
		/* nothing */
	} else if (!s9p_lpm_begin(priv)) {
		ret = -EBUSY;
	} else {
		if (want)
			s9p_lpm_enter(priv, want - 1);
		else
			s9p_lpm_exit(priv);
		if (want && !priv->lpm)
			priv->clock_first = true;
		priv->lpm = want;
		s9p_lpm_end(priv);
		dev_info(priv->drm.dev, "panel lpm %u\n", want);
		priv->bl_sent = -1;
		if (!want && priv->enabled && priv->bl && priv->bl_table &&
		    s9p_panel_brightness_table(priv, priv->bl->props.brightness, true))
			priv->bl_sent = priv->bl->props.brightness;
	}
	mutex_unlock(&priv->panel_lock);
	return ret;
}
static DEVICE_ATTR(aod_lpm, 0664, aod_lpm_show, aod_lpm_store);

static ssize_t aod_clock_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct s9p_decon *priv = dev_get_drvdata(dev);
	unsigned int x, y, hh, mm, ss, h24 = 0;
	int n, ret = count;
	enum { OFF, TIME, ANALOG } what;

	if (sysfs_streq(buf, "off")) {
		what = OFF;
	} else if ((n = sscanf(buf, "time %u %u %u %u", &hh, &mm, &ss, &h24)) >= 3) {
		what = TIME;
	} else if ((n = sscanf(buf, "analog %u %u %u %u %u %u", &x, &y, &hh, &mm, &ss, &h24)) >= 5) {
		what = ANALOG;
		if (x >= PANEL_W || y >= PANEL_H)
			return -EINVAL;
	} else {
		return -EINVAL;
	}
	if (what != OFF && (hh > 23 || mm > 59 || ss > 59))
		return -EINVAL;
	mutex_lock(&priv->panel_lock);
	if (!priv->lpm || (what == ANALOG && (!priv->sprite || !priv->kept)) ||
	    (what == TIME && !priv->clock_on)) {
		ret = -EAGAIN;		/* stock: only in ALPM, with an image */
	} else if (!s9p_lpm_begin(priv)) {
		ret = -EBUSY;
	} else {
		if (what == OFF) {
			KEY2_ON();
			LPM(0x77, 0x00, 0x00);	/* timer_off_ctrl */
			KEY2_OFF();
			priv->clock_on = false;
		} else if (what == TIME) {
			s9p_aod_set_time(priv, hh, mm, ss, h24);
			s9p_aod_analog_ctrl(priv, h24);
		} else {
			if (!priv->aod_entered)
				s9p_aod_enter(priv);
			if (!priv->sram_ok)
				s9p_aod_sprite_upload(priv);
			priv->clk_x = x;
			priv->clk_y = y;
			s9p_aod_set_time(priv, hh, mm, ss, h24);
			s9p_aod_analog_ctrl(priv, h24);
			priv->clock_on = true;
		}
		s9p_lpm_end(priv);
	}
	mutex_unlock(&priv->panel_lock);
	return ret;
}
static DEVICE_ATTR(aod_clock, 0220, NULL, aod_clock_store);

static ssize_t aod_sprite_write(struct file *file, struct kobject *kobj,
				const struct bin_attribute *attr, char *buf,
				loff_t off, size_t count)
{
	struct s9p_decon *priv = dev_get_drvdata(kobj_to_dev(kobj));

	if (off >= S9P_SPRITE_SIZE)
		return -EFBIG;
	count = min_t(size_t, count, S9P_SPRITE_SIZE - off);
	mutex_lock(&priv->panel_lock);
	if (!priv->sprite)
		priv->sprite = vzalloc(S9P_SPRITE_SIZE);	/* kept for the driver's life */
	if (priv->sprite) {
		memcpy(priv->sprite + off, buf, count);
		priv->sram_ok = false;	/* the new one goes at the next "analog" */
	}
	mutex_unlock(&priv->panel_lock);
	return priv->sprite ? count : -ENOMEM;
}
static const struct bin_attribute bin_attr_aod_sprite = {
	.attr = { .name = "aod_sprite", .mode = 0220 },
	.size = S9P_SPRITE_SIZE,
	.write = aod_sprite_write,
};

'''

edits = [
    ("#include <linux/sizes.h>\n", "#include <linux/sizes.h>\n#include <linux/vmalloc.h>\n"),
    ("""	bool kept;			/* blanked by aod_keep: the panel is awake */
};""", """	bool kept;			/* blanked by aod_keep: the panel is awake */
	/* zboot: panel lpm (patch-decon-lpm.py) */
	unsigned int lpm;		/* 0, or HLPM 1-4 (2/10/30/60 nit) */
	bool aod_entered;		/* the live clock's entry was sent */
	bool clock_on, clock_first;
	bool sram_ok;			/* the sprite is in the panel's side RAM */
	u16 clk_x, clk_y;
	u8 *sprite;			/* aod_sprite, S9P_SPRITE_SIZE */
};"""),
    ("static struct attribute *s9p_panel_attrs[] = {\n", CODE + "static struct attribute *s9p_panel_attrs[] = {\n"),
    ("""	&dev_attr_panel_cmd.attr,
	NULL
};""", """	&dev_attr_panel_cmd.attr,
	&dev_attr_aod_lpm.attr,
	&dev_attr_aod_clock.attr,
	NULL
};"""),
    ("""	&bin_attr_bl_table,
	NULL
};""", """	&bin_attr_bl_table,
	&bin_attr_aod_sprite,
	NULL
};"""),
    # a frame never goes with the live clock's timing set
    ("""	mutex_lock(&priv->panel_lock);
	if (priv->kept)
		priv->kept = false;	/* awake all along: no sleep-out */""",
     """	mutex_lock(&priv->panel_lock);
	if (priv->kept && priv->aod_entered && s9p_lpm_begin(priv)) {
		s9p_aod_leave(priv);	/* normal timing before any frame */
		s9p_lpm_end(priv);
	}
	if (priv->kept)
		priv->kept = false;	/* awake all along: no sleep-out */"""),
    # brightness held off in LPM
    ("	if (priv->enabled && !backlight_is_blank(bl)) {",
     "	if (priv->enabled && !priv->lpm && !backlight_is_blank(bl)) {"),
    ("	if (priv->enabled && priv->bl && priv->bl_table) {",
     "	if (priv->enabled && !priv->lpm && priv->bl && priv->bl_table) {"),
    ("""	if (priv->bl && priv->bl_table) {
		/* no frames before display-on: this one goes */""",
     """	if (priv->lpm) {
		/* in the panel's LPM (an AOD frame): its own level stays */
	} else if (priv->bl && priv->bl_table) {
		/* no frames before display-on: this one goes */"""),
    # a panel sleep ends LPM, the clock and the side RAM
    ("""static void s9p_panel_down(struct s9p_decon *priv)
{
""", """static void s9p_panel_down(struct s9p_decon *priv)
{
	/* zboot: panel lpm. Sleep-out reloads the registers; the side RAM goes */
	priv->lpm = 0;
	priv->aod_entered = false;
	priv->clock_on = false;
	priv->sram_ok = false;
"""),
]
for old, new in edits:
    if s.count(old) != 1:
        sys.exit("patch-decon-lpm: the driver changed near:\n" + old[:80])
    s = s.replace(old, new)
path.write_text(s)
print("patch-decon-lpm: applied")
