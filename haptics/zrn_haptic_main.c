// SPDX-License-Identifier: GPL-2.0
/*
 * The Galaxy S9's vibration motor under mainline, as a rumble input device.
 *
 * The motor is a linear resonant actuator driven by the MAX77705's motor
 * driver, which takes a PWM from the SoC and divides it by 128: 20.48 kHz in,
 * the motor's 160 Hz out. The power chip's core driver (MFD_MAX77705) already
 * makes a "max77705-haptic" platform device; mainline's own driver for it
 * (max77693-haptic) wants a PWM and a regulator from a device tree that
 * describes neither here (there is no PWM controller in ours), so this one
 * does the little there is to do by itself, from the stock tree's values
 * (stock/twrp-live.dts: haptic, vib-pwm, motor-boost-en):
 *
 *  - the PWM is the SoC's timer 1 (0x10510000), out on gpp3-1 (function 2);
 *    its clock is the 26 MHz oscillator (the boot loader leaves a prescaler
 *    of 26 there), so 1270 counts are the stock period of 48828 ns;
 *  - the motor's supply is a boost switched on by gpg1-7;
 *  - MCONFIG (0x10) = 0x82: LRA, PWM divided by 128; bit 6 runs the motor;
 *    MAINCTRL1 (0x02) bit 7 is the driver's bias.
 *
 * The chip drives the motor in proportion to how far the duty is from one
 * half, with the sign: measured with the phone's microphone, a duty as far
 * below a half as another is above it gives the same buzz in the opposite
 * phase. So a duty below a half pushes against the motion: a brake.
 *
 * Shape of a pulse (v3). The motor is a mass on a spring: its buzz builds up
 * and dies away with a time constant (tau_us, about 14 ms here, measured), so
 * a plain on/off drive is felt for about 30 ms longer than it is driven (the
 * "ringing"). v2 tried to hide that with a fixed 8 ms overdrive and a fixed
 * 10 ms reversed drive, but a fixed brake is the wrong length for almost every
 * pulse: once the mass has stopped, the rest of it drives it again the other
 * way, and the motor rang on for longer than with no brake at all.
 *
 * This version keeps a model of the motor's amplitude (first order, tau_us)
 * and plans each pulse from it, so that what is felt is what was asked for:
 *
 *  - overdrive: full drive (od_pct) only until the amplitude has reached the
 *    strength asked for, so short ticks start at once and nothing ever goes
 *    stronger than asked;
 *  - sustain: the strength asked for, until the pulse's length is up;
 *  - brake: reversed drive (brake_pct) for just as long as the model says the
 *    mass takes to stop (at most brake_max_ms), then nothing.
 *
 * The chip is switched off (MCONFIG bit 6) idle_ms after a pulse; it is 0 by
 * default, because the chip, enabled and still, hums faintly (half duty is
 * not quite its zero; zero_trim moves the zero in PWM counts) and starting it
 * again costs only an I2C write (about 0.2 ms). A pulse is a rumble effect
 * whose replay.length is its length in ms; the strength is the larger of its
 * two magnitudes. Playing it again restarts it from where the motor is;
 * stopping it brakes at once. The knobs are module parameters, writable under
 * /sys/module, and /sys/bus/platform/devices/.../stats tells how each pulse
 * was played.
 */
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sched.h>
#include <linux/spinlock.h>

#ifdef ZH_DEV
#define ZH_DRIVER	"zrn-haptic-dev"	/* a second driver, to try a build beside the loaded one */
#else
#define ZH_DRIVER	"zrn-haptic"
#endif

#define PWM_BASE	0x10510000
#define TCFG0		0x00
#define TCFG1		0x04
#define TCON		0x08
#define TCNTB1		0x18
#define TCMPB1		0x1c
#define TCON_T1_START	BIT(8)
#define TCON_T1_MANUAL	BIT(9)
#define TCON_T1_INVERT	BIT(10)
#define TCON_T1_RELOAD	BIT(11)
#define PERIOD_COUNTS	1270	/* 26 MHz / 1270 = 20.47 kHz; / 128 = 160 Hz */
#define DUTY_PERCENT	95

#define PINS_BASE	0x10430000
#define GPP3		0x060
#define GPG1		0x0a0
#define PIN_CON		0x0
#define PIN_DAT		0x4
#define PIN_PUD		0x8

#define REG_MAINCTRL1	0x02
#define MAINCTRL1_BIASEN BIT(7)
#define REG_MCONFIG	0x10
#define MCONFIG_LRA_DIV128 0x82
#define MCONFIG_MEN	BIT(6)

#define MAG_MAX		0xffff
#define ONE		0x10000		/* full drive, in the model's units (Q16) */
#define LOG_N		8

static unsigned int tau_us = 14000;	/* the motor's time constant */
static unsigned int od_pct = 100;	/* overdrive level, % of full; 0 none */
static unsigned int brake_pct = 100;	/* brake level, % of full; 0 none */
static unsigned int brake_max_ms = 15;	/* the longest a brake may be */
static unsigned int idle_ms;		/* the chip stays enabled this long after a pulse */
static int zero_trim;			/* PWM counts added to a half duty */
module_param(tau_us, uint, 0644);
module_param(od_pct, uint, 0644);
module_param(brake_pct, uint, 0644);
module_param(brake_max_ms, uint, 0644);
module_param(idle_ms, uint, 0644);
module_param(zero_trim, int, 0644);
MODULE_PARM_DESC(tau_us, "the motor's time constant (build-up and ring-down), us");
MODULE_PARM_DESC(od_pct, "drive while the motor builds up to strength, % of full (0: none)");
MODULE_PARM_DESC(brake_pct, "reversed drive that stops the motor, % of full (0: none)");
MODULE_PARM_DESC(brake_max_ms, "the longest a brake may last, ms");
MODULE_PARM_DESC(idle_ms, "time the motor chip stays enabled after a pulse, ms");
MODULE_PARM_DESC(zero_trim, "PWM counts added to the half duty where the chip is still");

enum zh_phase { ZH_IDLE, ZH_OD, ZH_SUSTAIN, ZH_BRAKE };

struct zh_log {
	u32 req_ms, od_us, drive_us, brake_us;
	s64 latency_us;
	s64 total_us;		/* first drive to the end of the brake */
	u32 mag;
	s32 a_start, a_end;	/* the model's amplitude as it began and as the drive ended, Q16 */
};

struct zrn_haptic {
	struct device *dev;
	struct regmap *regmap;
	void __iomem *pwm, *pins;
	struct input_dev *input;

	struct kthread_worker *worker;
	struct kthread_work start_work;
	struct kthread_delayed_work off_work;
	struct hrtimer timer;
	raw_spinlock_t lock;		/* the fields below; taken in hard irq */
	enum zh_phase phase;
	bool chip_on;			/* MCONFIG's MEN is set */
	bool start_queued;
	bool pwm_running;
	bool dying;			/* being removed: nothing more starts */
	u32 mag, req_ms, len_us;	/* the pulse asked for: strength (0..MAG_MAX), length */
	s32 u_od, u_sus, u_brake;	/* drive levels, Q16 (the brake's negative) */
	u32 t_od, t_sus, t_brake;	/* us */
	/* the model: the amplitude was a0 at t0, driven towards u since */
	s32 a0, u;
	ktime_t t0;
	ktime_t phase_end, requested, started, idle_since;
	struct zh_log log[LOG_N];
	unsigned int log_n;
	unsigned long pulses;
};

static const struct regmap_config zrn_haptic_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
};

/* ---- the model -------------------------------------------------------- */

/* e^-(us / tau_us), Q16 */
static u32 zh_decay(u64 us)
{
	u32 tau = max(tau_us, 500U);
	u64 x = div_u64(us << 16, tau);	/* us / tau, Q16 */
	u64 f = x & 0xffff, r = ONE, term = ONE;
	unsigned int n = x >> 16, k;

	if (n >= 12)
		return 0;
	/* e^-f by its series (f < 1: six terms are good to 2e-4) */
	for (k = 1; k <= 6; k++) {
		term = term * f / ONE / k;
		r = k & 1 ? r - term : r + term;
	}
	while (n--)
		r = r * 24109 / ONE;	/* e^-1 */
	return r;
}

/* the amplitude after us of drive u, from a */
static s32 zh_after(s32 a, s32 u, u64 us)
{
	return u + (s32)div_s64((s64)(a - u) * zh_decay(us), ONE);
}

/* how long drive u takes to bring the amplitude from a to target (which
 * lies between them); at most cap */
static u32 zh_time_to(s32 a, s32 u, s32 target, u32 cap)
{
	u32 lo = 0, hi = cap;

	if ((u > a) ? (zh_after(a, u, cap) < target) : (zh_after(a, u, cap) > target))
		return cap;
	while (hi - lo > 20) {
		u32 mid = (lo + hi) / 2;
		s32 v = zh_after(a, u, mid);

		if ((u > a) ? (v < target) : (v > target))
			lo = mid;
		else
			hi = mid;
	}
	return hi;
}

/* the model's amplitude now; with h->lock */
static s32 zh_amp_now(struct zrn_haptic *h, ktime_t now)
{
	return zh_after(h->a0, h->u, max_t(s64, ktime_us_delta(now, h->t0), 0));
}

/* ---- the PWM ---------------------------------------------------------- */

/* level: -MAG_MAX..MAG_MAX. Zero is a half duty, where the chip is still; a
 * negative level is the same distance from a half on the other side, which
 * drives the motor the other way. */
static void zrn_haptic_pwm(struct zrn_haptic *h, int level)
{
	u32 tcon = readl(h->pwm + TCON) & ~(TCON_T1_START | TCON_T1_MANUAL | TCON_T1_RELOAD);
	u32 half = PERIOD_COUNTS / 2 + clamp(zero_trim, -60, 60);
	u32 span = PERIOD_COUNTS * DUTY_PERCENT / 100 - PERIOD_COUNTS / 2;
	u32 duty;

	/* as pwm-samsung does it: the count, and where the output changes; with
	 * the inverter on, the time below the compare value is the high time.
	 * The chip divides this PWM by 128 for the motor's 160 Hz and takes its
	 * strength from how far the duty is from one half. */
	if (level >= 0)
		duty = half + span * (u32)min(level, MAG_MAX) / MAG_MAX;
	else
		duty = half - span * (u32)min(-level, MAG_MAX) / MAG_MAX;
	writel(PERIOD_COUNTS - 1 - duty, h->pwm + TCMPB1);
	if (h->pwm_running)
		return;	/* taken at the next period (49 us), without restarting the count */
	writel(readl(h->pwm + TCFG0) & ~0xffU, h->pwm + TCFG0);	/* timers 0 and 1: no prescaler */
	writel(readl(h->pwm + TCFG1) & ~0xf0U, h->pwm + TCFG1);	/* timer 1: no divider */
	writel(PERIOD_COUNTS - 1, h->pwm + TCNTB1);
	writel(tcon | TCON_T1_INVERT | TCON_T1_MANUAL, h->pwm + TCON);
	writel(tcon | TCON_T1_INVERT | TCON_T1_RELOAD | TCON_T1_START, h->pwm + TCON);
	h->pwm_running = true;
}

static void zrn_haptic_pwm_off(struct zrn_haptic *h)
{
	u32 tcon = readl(h->pwm + TCON) & ~(TCON_T1_START | TCON_T1_MANUAL | TCON_T1_RELOAD);

	writel(tcon | TCON_T1_INVERT, h->pwm + TCON);	/* stopped, the output low */
	h->pwm_running = false;
}

/* ---- playing a pulse -------------------------------------------------- */

/* with h->lock: drive u from now on (the model follows), for us */
static void zh_drive(struct zrn_haptic *h, enum zh_phase phase, s32 u, u32 us, ktime_t now)
{
	h->a0 = zh_amp_now(h, now);
	h->u = u;
	h->t0 = now;
	h->phase = phase;
	zrn_haptic_pwm(h, clamp(u, -MAG_MAX, MAG_MAX));
	h->phase_end = ktime_add_us(now, us);
	hrtimer_start(&h->timer, ns_to_ktime(us * 1000ULL), HRTIMER_MODE_REL_HARD);
}

static void zh_log_end(struct zrn_haptic *h, ktime_t now)
{
	struct zh_log *l = &h->log[(h->log_n - 1) % LOG_N];

	if (h->log_n && !l->total_us)
		l->total_us = ktime_us_delta(now, h->started);
}

/* with h->lock: the motor left alone (the model decays) */
static void zh_rest(struct zrn_haptic *h, ktime_t now, bool stopped)
{
	h->a0 = stopped ? 0 : zh_amp_now(h, now);
	h->u = 0;
	h->t0 = now;
	h->phase = ZH_IDLE;
	zrn_haptic_pwm(h, 0);
	zh_log_end(h, now);
	h->idle_since = now;
	kthread_queue_delayed_work(h->worker, &h->off_work, msecs_to_jiffies(idle_ms));
}

/* with h->lock: brake from where the motor is now, or rest if there is nothing to stop */
static void zh_brake(struct zrn_haptic *h, ktime_t now)
{
	s32 a = zh_amp_now(h, now);
	u32 t = 0;

	if (a > ONE / 200 && h->u_brake < 0)
		t = zh_time_to(a, h->u_brake, 0, brake_max_ms * 1000);
	if (h->log_n) {
		struct zh_log *l = &h->log[(h->log_n - 1) % LOG_N];

		l->a_end = a;
		l->brake_us = t;
	}
	if (t >= 50)
		zh_drive(h, ZH_BRAKE, h->u_brake, t, now);
	else
		zh_rest(h, now, false);
}

/* hrtimer, hard irq: the next part of the pulse, or the end of it */
static enum hrtimer_restart zh_timer(struct hrtimer *t)
{
	struct zrn_haptic *h = container_of(t, struct zrn_haptic, timer);
	ktime_t now = ktime_get();
	unsigned long flags;

	raw_spin_lock_irqsave(&h->lock, flags);
	if (h->phase == ZH_IDLE || ktime_before(now, h->phase_end))
		goto out;	/* a pulse started again meanwhile: its own timer is queued */
	switch (h->phase) {
	case ZH_OD:
		if (h->t_sus) {
			zh_drive(h, ZH_SUSTAIN, h->u_sus, h->t_sus, now);
			break;
		}
		fallthrough;
	case ZH_SUSTAIN:
		zh_brake(h, now);
		break;
	default:
		zh_rest(h, now, true);
	}
out:
	raw_spin_unlock_irqrestore(&h->lock, flags);
	return HRTIMER_NORESTART;
}

/* with h->lock held and the chip enabled: plan the pulse from where the motor
 * is now, and drive it */
static void zh_begin(struct zrn_haptic *h)
{
	struct zh_log *l = &h->log[h->log_n % LOG_N];
	ktime_t now = ktime_get();
	s32 a = zh_amp_now(h, now);
	s32 m = h->mag + (h->mag >> 15);	/* 0..ONE */
	s32 od = min_t(u64, (u64)ONE * od_pct / 100, ONE);
	u32 len = h->len_us;

	h->u_sus = m;
	h->u_brake = -(s32)min_t(u64, (u64)ONE * brake_pct / 100, ONE);
	/* full drive until the amplitude is up to strength: the time the model
	 * says that takes, but never longer than the pulse */
	h->u_od = od;
	h->t_od = od > m && a < m ? zh_time_to(a, od, m, len) : 0;
	h->t_sus = len - h->t_od;

	h->pulses++;
	h->log_n++;
	h->started = now;
	memset(l, 0, sizeof(*l));
	l->req_ms = h->req_ms;
	l->mag = h->mag;
	l->od_us = h->t_od;
	l->drive_us = h->t_sus;
	l->latency_us = ktime_us_delta(now, h->requested);
	l->a_start = a;
	if (h->t_od)
		zh_drive(h, ZH_OD, h->u_od, h->t_od, now);
	else
		zh_drive(h, ZH_SUSTAIN, h->u_sus, h->t_sus, now);
}

/* the worker: enable the chip if it is not, then start what is planned */
static void zh_start_work(struct kthread_work *work)
{
	struct zrn_haptic *h = container_of(work, struct zrn_haptic, start_work);
	unsigned long flags;
	bool need;

	raw_spin_lock_irqsave(&h->lock, flags);
	need = !h->chip_on;
	raw_spin_unlock_irqrestore(&h->lock, flags);
	if (need)
		regmap_write(h->regmap, REG_MCONFIG, MCONFIG_LRA_DIV128 | MCONFIG_MEN);

	raw_spin_lock_irqsave(&h->lock, flags);
	h->chip_on = true;
	if (h->start_queued && !h->dying) {
		h->start_queued = false;
		zh_begin(h);
	}
	raw_spin_unlock_irqrestore(&h->lock, flags);
}

static void zh_off_work(struct kthread_work *work)
{
	struct zrn_haptic *h = container_of(work, struct zrn_haptic, off_work.work);
	unsigned long flags;
	bool off;

	raw_spin_lock_irqsave(&h->lock, flags);
	off = h->phase == ZH_IDLE && h->chip_on && !h->start_queued;
	if (off) {
		s64 left = (s64)idle_ms * 1000 - ktime_us_delta(ktime_get(), h->idle_since);

		if (left > 1000) {	/* a pulse ended since this was queued */
			kthread_queue_delayed_work(h->worker, &h->off_work, usecs_to_jiffies(left));
			off = false;
		} else {
			h->chip_on = false;
		}
	}
	raw_spin_unlock_irqrestore(&h->lock, flags);
	if (off) {
		regmap_write(h->regmap, REG_MCONFIG, MCONFIG_LRA_DIV128);
		raw_spin_lock_irqsave(&h->lock, flags);
		if (!h->chip_on)
			zrn_haptic_pwm_off(h);
		raw_spin_unlock_irqrestore(&h->lock, flags);
	}
}

static int zrn_haptic_upload(struct input_dev *input, struct ff_effect *effect,
			     struct ff_effect *old)
{
	return effect->type == FF_RUMBLE ? 0 : -EINVAL;
}

/* input event_lock held (irqs off): start or stop an effect */
static int zrn_haptic_playback(struct input_dev *input, int id, int value)
{
	struct zrn_haptic *h = input_get_drvdata(input);
	struct ff_effect *e = &input->ff->effects[id];
	unsigned long flags;
	u32 mag = max(e->u.rumble.strong_magnitude, e->u.rumble.weak_magnitude);
	u32 len = e->replay.length ? e->replay.length : 20;

	raw_spin_lock_irqsave(&h->lock, flags);
	if (!value || !mag) {
		/* stopped: brake now if it is driving; a start not yet made is dropped */
		h->start_queued = false;
		if (h->phase == ZH_OD || h->phase == ZH_SUSTAIN)
			zh_brake(h, ktime_get());
	} else if (!h->dying) {
		h->mag = mag;
		h->req_ms = len;
		h->len_us = min(len, 5000U) * 1000;
		h->requested = ktime_get();
		if (h->chip_on) {
			zh_begin(h);	/* already enabled: within this call */
		} else if (!h->start_queued) {
			h->start_queued = true;
			kthread_queue_work(h->worker, &h->start_work);
		}
	}
	raw_spin_unlock_irqrestore(&h->lock, flags);
	return 0;
}

static ssize_t stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct zrn_haptic *h = dev_get_drvdata(dev);
	unsigned long flags;
	int n = 0, i;

	raw_spin_lock_irqsave(&h->lock, flags);
	n += sysfs_emit_at(buf, n, "pulses %lu  tau_us %u od_pct %u brake_pct %u brake_max_ms %u idle_ms %u zero_trim %d\n",
			   h->pulses, tau_us, od_pct, brake_pct, brake_max_ms, idle_ms, zero_trim);
	n += sysfs_emit_at(buf, n, "newest first: asked_ms mag od_us drive_us brake_us start_latency_us first_drive_to_end_us amp_at_start%% amp_at_drive_end%%\n");
	for (i = 1; i <= LOG_N && i <= h->log_n; i++) {
		struct zh_log *l = &h->log[(h->log_n - i) % LOG_N];

		n += sysfs_emit_at(buf, n, "%u %u %u %u %u %lld %lld %d %d\n", l->req_ms, l->mag, l->od_us,
				   l->drive_us, l->brake_us, l->latency_us, l->total_us,
				   l->a_start * 100 / ONE, l->a_end * 100 / ONE);
	}
	raw_spin_unlock_irqrestore(&h->lock, flags);
	return n;
}
static DEVICE_ATTR_RO(stats);

static void zrn_haptic_close(struct input_dev *input)
{
	/* the last one to close it leaves the motor still: flush has stopped the effects */
}

static int zrn_haptic_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct i2c_client *i2c = to_i2c_client(dev->parent);
	struct zrn_haptic *h;
	u32 v;
	int ret;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->dev = dev;
	h->regmap = devm_regmap_init_i2c(i2c, &zrn_haptic_regmap);
	if (IS_ERR(h->regmap))
		return dev_err_probe(dev, PTR_ERR(h->regmap), "no regmap\n");
	h->pwm = devm_ioremap(dev, PWM_BASE, 0x100);
	h->pins = devm_ioremap(dev, PINS_BASE, 0x100);
	if (!h->pwm || !h->pins)
		return -ENOMEM;
	raw_spin_lock_init(&h->lock);
	hrtimer_setup(&h->timer, zh_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
	h->t0 = ktime_get();
	h->worker = kthread_run_worker(0, "zrn-haptic");
	if (IS_ERR(h->worker))
		return PTR_ERR(h->worker);
	sched_set_fifo(h->worker->task);	/* a pulse starts the moment it is asked for */
	kthread_init_work(&h->start_work, zh_start_work);
	kthread_init_delayed_work(&h->off_work, zh_off_work);

	/* gpg1-7: an output, high (the motor's boost); gpp3-1: the timer's output */
	writel(readl(h->pins + GPG1 + PIN_DAT) | BIT(7), h->pins + GPG1 + PIN_DAT);
	v = readl(h->pins + GPG1 + PIN_CON);
	writel((v & ~(0xfU << 28)) | (1U << 28), h->pins + GPG1 + PIN_CON);
	zrn_haptic_pwm_off(h);
	v = readl(h->pins + GPP3 + PIN_CON);
	writel((v & ~(0xfU << 4)) | (2U << 4), h->pins + GPP3 + PIN_CON);
	v = readl(h->pins + GPP3 + PIN_PUD);
	writel((v & ~(0xfU << 4)) | (1U << 4), h->pins + GPP3 + PIN_PUD);	/* pulled down */

	ret = regmap_update_bits(h->regmap, REG_MAINCTRL1, MAINCTRL1_BIASEN, MAINCTRL1_BIASEN);
	if (!ret)
		ret = regmap_write(h->regmap, REG_MCONFIG, MCONFIG_LRA_DIV128);
	if (ret) {
		kthread_destroy_worker(h->worker);
		return dev_err_probe(dev, ret, "the power chip did not take the motor's settings\n");
	}

	h->input = input_allocate_device();
	if (!h->input) {
		kthread_destroy_worker(h->worker);
		return -ENOMEM;
	}
	h->input->name = "zrn-haptic";
	h->input->id.bustype = BUS_I2C;
	h->input->dev.parent = dev;
	h->input->close = zrn_haptic_close;
	input_set_drvdata(h->input, h);
	input_set_capability(h->input, EV_FF, FF_RUMBLE);
	ret = input_ff_create(h->input, 16);
	if (ret)
		goto err;
	h->input->ff->upload = zrn_haptic_upload;
	h->input->ff->playback = zrn_haptic_playback;
	platform_set_drvdata(pdev, h);
	ret = input_register_device(h->input);
	if (ret)
		goto err;
	device_create_file(dev, &dev_attr_stats);
	dev_info(dev, "vibration motor ready (160 Hz, %d %% duty, tau %u us, shaped pulses)\n",
		 DUTY_PERCENT, tau_us);
	return 0;
err:
	input_free_device(h->input);
	kthread_destroy_worker(h->worker);
	return dev_err_probe(dev, ret, "no rumble device\n");
}

static void zrn_haptic_remove(struct platform_device *pdev)
{
	struct zrn_haptic *h = platform_get_drvdata(pdev);

	unsigned long flags;

	/* nothing can ask for a pulse once the input device is gone, and nothing
	 * queued may start one; then the timer, and last the worker */
	input_unregister_device(h->input);
	device_remove_file(&pdev->dev, &dev_attr_stats);
	raw_spin_lock_irqsave(&h->lock, flags);
	h->dying = true;
	raw_spin_unlock_irqrestore(&h->lock, flags);
	kthread_flush_worker(h->worker);
	hrtimer_cancel(&h->timer);
	kthread_cancel_delayed_work_sync(&h->off_work);
	kthread_destroy_worker(h->worker);
	regmap_write(h->regmap, REG_MCONFIG, MCONFIG_LRA_DIV128);
	zrn_haptic_pwm_off(h);
}

static const struct platform_device_id zrn_haptic_ids[] = {
	{ "max77705-haptic" },
	{ }
};
MODULE_DEVICE_TABLE(platform, zrn_haptic_ids);

static struct platform_driver zrn_haptic_driver = {
	.driver = { .name = ZH_DRIVER },
	.probe = zrn_haptic_probe,
	.remove = zrn_haptic_remove,
	.id_table = zrn_haptic_ids,
};
module_platform_driver(zrn_haptic_driver);

MODULE_DESCRIPTION("Galaxy S9 vibration motor (MAX77705 LRA driver, SoC timer PWM)");
MODULE_LICENSE("GPL");
