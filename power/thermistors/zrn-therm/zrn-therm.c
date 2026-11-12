// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-therm: the S9's thermistors on the Exynos 9810's own ADC, read as
 * stock reads them, and the battery's temperature written into the MAX77705
 * fuel gauge's Temp register, as stock writes it (power/thermistors/README.md).
 *
 * Without it nothing measures the battery's temperature on ztOS: stock left
 * the gauge expecting the host to write Temp (Config.TEX), and the register
 * holds Android's last value (power/battery-cycles.md).
 *
 * Writes, each approved by the owner (2026-11-12), and nothing else:
 *   ADC 0x14230000: CON1 0x00 (0x4, then 0x2; bit 0 to start), CON2 0x04
 *   (0x60, the channel in bits 3:0), INT_EN 0x10 (1 while converting, 0 after:
 *   only as the done flag, polled; no interrupt handler), INT_ST 0x14 (1 to
 *   clear). The ADC's bus clock gate in CMU_CMGP (0x14202018 and 0x1420201c,
 *   ADC_CMGP PCLK_S0/S1): CG_VAL (bit 21) set only if it reads closed, and
 *   cleared again on unload; it read open (0x00200000) on the first run.
 *   Fuel gauge (I2C 1-0036, through the max17042 driver's regmap): Temp 0x08.
 *
 * It reads the gate before it touches the ADC: a read of a gated block hangs
 * the bus. Sampling: every fast_ms (5 s) while a charger or pad is in or the
 * screen is on, else every slow_ms (60 s); on a freezable workqueue, so it
 * never wakes the phone, and once more straight after resume.
 *
 * /sys/kernel/zrn_therm/: battery usb charger ap wifi pa (tenths of a degree
 * C; reading one fails with ENODATA when it is unknown: never read, an ADC
 * error, outside its table, or stale), raw (every channel), status, and
 * sample (write anything: sample now). zrn-charge reads battery.
 */
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/kobject.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/suspend.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "therm-tables.h"
#include "therm-fresh.h"

#define CMU_CMGP_GATES		0x14202018	/* ADC_CMGP PCLK_S0; S1 at + 4 */
#define GATE_MANUAL		BIT(20)
#define GATE_CG_VAL		BIT(21)

#define ADC_BASE		0x14230000
#define ADC_CON1		0x00
#define ADC_CON2		0x04
#define ADC_DAT			0x08
#define ADC_INT_EN		0x10
#define ADC_INT_ST		0x14
#define CON1_SOFT_RESET		BIT(2)
#define CON1_SOFT_NON_RESET	BIT(1)
#define CON1_START		BIT(0)
#define CON2_C_TIME(x)		(((x) & 7) << 4)
#define CON2_ACH_MASK		0xf
#define DAT_MASK		0xfff

#define FG_TEMP			0x08

static bool open_gate = true;
module_param(open_gate, bool, 0644);
MODULE_PARM_DESC(open_gate, "open the ADC's clock gate if it reads closed (CG_VAL only; closed again on unload)");
static bool write_gauge = true;
module_param(write_gauge, bool, 0644);
MODULE_PARM_DESC(write_gauge, "write the battery temperature into the fuel gauge's Temp register");
static bool battery_only;
module_param(battery_only, bool, 0644);
MODULE_PARM_DESC(battery_only, "convert the battery's channel only (a first test)");
static bool sampling = true;
module_param(sampling, bool, 0644);
MODULE_PARM_DESC(sampling, "sample on a timer (0: only when sample is written)");
static unsigned int fast_ms = 5000, slow_ms = 60000;
module_param(fast_ms, uint, 0644);
module_param(slow_ms, uint, 0644);
static char *fg_dev = "1-0036";
module_param(fg_dev, charp, 0444);
static char *panel = "s9p-panel";
module_param(panel, charp, 0444);

static DEFINE_MUTEX(lock);
static void __iomem *adc, *gates;
static struct delayed_work work;
static struct kobject *kobj;

static struct {
	int raw;		/* trimmed mean of THERM_SAMPLES conversions; -1 none */
	int deci;
	struct therm_fresh fresh;
} chan[THERM_N_CHANNELS];

static struct {
	unsigned long samples, errors;
	u64 ns_total, ns_last;
	int last_err;		/* 0, or why the last attempt failed */
	int gauge_err;		/* the last gauge write's result */
	unsigned int gauge_reg;	/* what was written */
	u32 gate[2];		/* the gate registers as last read */
	unsigned int period;	/* ms, the current period */
	bool gate_opened[2];	/* closed when found: closed again on unload */
} st;

static long long now_ms(void)
{
	return ktime_to_ms(ktime_get_boottime());
}

/* 0 when the ADC's bus clock runs (or is left to the hardware), else -EACCES */
static int gate_check(void)
{
	int i;

	for (i = 0; i < 2; i++) {
		u32 v = readl(gates + 4 * i);

		st.gate[i] = v;
		if ((v & GATE_MANUAL) && !(v & GATE_CG_VAL)) {
			if (!open_gate) {
				pr_warn_ratelimited("zrn-therm: ADC clock gate %d closed (0x%08x); not opening it (open_gate=0)\n",
						    i, v);
				return -EACCES;
			}
			writel(v | GATE_CG_VAL, gates + 4 * i);
			st.gate_opened[i] = true;
			pr_info("zrn-therm: ADC clock gate %d was closed (0x%08x): opened, now 0x%08x\n",
				i, v, readl(gates + 4 * i));
		}
	}
	return 0;
}

static int convert(int ch, int *raw)
{
	u32 con2 = readl(adc + ADC_CON2);
	int i;

	writel((con2 & ~CON2_ACH_MASK) | ch, adc + ADC_CON2);
	writel(1, adc + ADC_INT_ST);
	writel(readl(adc + ADC_CON1) | CON1_START, adc + ADC_CON1);
	for (i = 0; i < 200; i++) {	/* 2 ms at most; a conversion takes microseconds */
		if (readl(adc + ADC_INT_ST) & 1) {
			*raw = readl(adc + ADC_DAT) & DAT_MASK;
			writel(1, adc + ADC_INT_ST);
			return 0;
		}
		udelay(10);
	}
	return -ETIMEDOUT;
}

static int write_gauge_temp(int deci)
{
	struct device *dev = bus_find_device_by_name(&i2c_bus_type, NULL, fg_dev);
	struct regmap *map;
	int ret = -ENODEV;

	if (!dev)
		return ret;
	map = dev_get_regmap(dev, "max17042");
	if (map) {
		st.gauge_reg = therm_gauge_reg(deci);
		ret = regmap_write(map, FG_TEMP, st.gauge_reg);
	}
	put_device(dev);
	return ret;
}

static void sample_locked(void)
{
	u64 t0 = ktime_get_ns();
	int c, i, ret, s[THERM_SAMPLES];

	ret = gate_check();
	if (!ret) {
		writel(CON1_SOFT_RESET, adc + ADC_CON1);
		writel(CON1_SOFT_NON_RESET, adc + ADC_CON1);
		writel(CON2_C_TIME(6), adc + ADC_CON2);
		writel(1, adc + ADC_INT_EN);
	}
	for (c = 0; c < THERM_N_CHANNELS; c++) {
		int r = ret;

		if (battery_only && c > 0) {
			therm_fresh_set(&chan[c].fresh, now_ms(), 0);
			chan[c].raw = -1;
			continue;
		}
		for (i = 0; !r && i < THERM_SAMPLES; i++)
			r = convert(therm_channels[c].ch, &s[i]);
		if (!r) {
			chan[c].raw = therm_trim_mean(s, THERM_SAMPLES);
			r = therm_decode(&therm_channels[c].t, chan[c].raw, &chan[c].deci) ? -ERANGE : 0;
		} else {
			chan[c].raw = -1;
		}
		therm_fresh_set(&chan[c].fresh, now_ms(), !r);
		if (r && c == 0) {
			st.last_err = r;
			st.errors++;
		} else if (c == 0) {
			st.last_err = 0;
		}
	}
	if (!ret) {
		writel(readl(adc + ADC_CON2) & ~CON2_C_TIME(7), adc + ADC_CON2);
		writel(0, adc + ADC_INT_EN);
	}
	st.ns_last = ktime_get_ns() - t0;
	st.ns_total += st.ns_last;
	st.samples++;

	/* only a good battery reading reaches the gauge */
	if (write_gauge && chan[0].fresh.ok)
		st.gauge_err = write_gauge_temp(chan[0].deci);
}

static bool screen_on(void)
{
	struct backlight_device *bd = backlight_device_get_by_name(panel);
	bool on;

	if (!bd)
		return false;
	on = bd->props.brightness > 0 && bd->props.power == BACKLIGHT_POWER_ON;
	put_device(&bd->dev);
	return on;
}

static void sample_work(struct work_struct *w)
{
	mutex_lock(&lock);
	sample_locked();
	st.period = (power_supply_is_system_supplied() > 0 || screen_on()) ? fast_ms : slow_ms;
	mutex_unlock(&lock);
	if (sampling)
		queue_delayed_work(system_freezable_power_efficient_wq, &work, msecs_to_jiffies(st.period));
}

static int pm_event(struct notifier_block *nb, unsigned long event, void *unused)
{
	if (event == PM_POST_SUSPEND && sampling)
		mod_delayed_work(system_freezable_power_efficient_wq, &work, 0);
	return NOTIFY_DONE;
}
static struct notifier_block pm_nb = { .notifier_call = pm_event };

static ssize_t chan_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	int c, ret = -ENODATA;

	mutex_lock(&lock);
	for (c = 0; c < THERM_N_CHANNELS; c++)
		if (!strcmp(a->attr.name, therm_channels[c].name) &&
		    therm_fresh_ok(&chan[c].fresh, now_ms(), st.period ?: slow_ms))
			ret = sysfs_emit(buf, "%d\n", chan[c].deci);
	mutex_unlock(&lock);
	return ret;
}

static ssize_t raw_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	int c, n = 0;

	mutex_lock(&lock);
	for (c = 0; c < THERM_N_CHANNELS; c++) {
		bool ok = therm_fresh_ok(&chan[c].fresh, now_ms(), st.period ?: slow_ms);

		n += sysfs_emit_at(buf, n, "%s ch%d raw %d %s", therm_channels[c].name, therm_channels[c].ch,
				   chan[c].raw, ok ? "" : "unknown");
		if (ok)
			n += sysfs_emit_at(buf, n, "%s%d.%d C", chan[c].deci < 0 ? "-" : "",
					   abs(chan[c].deci) / 10, abs(chan[c].deci) % 10);
		n += sysfs_emit_at(buf, n, "\n");
	}
	mutex_unlock(&lock);
	return n;
}

static ssize_t status_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	ssize_t n;

	mutex_lock(&lock);
	n = sysfs_emit(buf, "samples %lu errors %lu last_err %d period_ms %u cost_us_last %llu cost_us_avg %llu\n"
		       "gate 0x%08x 0x%08x%s\ngauge_write %d reg 0x%04x\n",
		       st.samples, st.errors, st.last_err, st.period, st.ns_last / 1000,
		       st.samples ? st.ns_total / st.samples / 1000 : 0, st.gate[0], st.gate[1],
		       (st.gate_opened[0] || st.gate_opened[1]) ? " (opened by zrn-therm)" : "", st.gauge_err, st.gauge_reg);
	mutex_unlock(&lock);
	return n;
}

static ssize_t sample_store(struct kobject *k, struct kobj_attribute *a, const char *buf, size_t count)
{
	mutex_lock(&lock);
	sample_locked();
	if (!st.period)
		st.period = slow_ms;
	mutex_unlock(&lock);
	return count;
}

static struct kobj_attribute attrs[] = {
	__ATTR(battery, 0444, chan_show, NULL), __ATTR(usb, 0444, chan_show, NULL),
	__ATTR(charger, 0444, chan_show, NULL), __ATTR(ap, 0444, chan_show, NULL),
	__ATTR(wifi, 0444, chan_show, NULL), __ATTR(pa, 0444, chan_show, NULL),
	__ATTR(raw, 0444, raw_show, NULL), __ATTR(status, 0444, status_show, NULL),
	__ATTR(sample, 0200, NULL, sample_store),
};
static struct attribute *attr_list[ARRAY_SIZE(attrs) + 1];
static const struct attribute_group group = { .attrs = attr_list };

static int __init zrn_therm_init(void)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(attrs); i++)
		attr_list[i] = &attrs[i].attr;
	for (i = 0; i < THERM_N_CHANNELS; i++)
		chan[i].raw = -1;
	gates = ioremap(CMU_CMGP_GATES, 8);
	adc = ioremap(ADC_BASE, 0x100);
	if (!gates || !adc) {
		ret = -ENOMEM;
		goto unmap;
	}
	kobj = kobject_create_and_add("zrn_therm", kernel_kobj);
	if (!kobj) {
		ret = -ENOMEM;
		goto unmap;
	}
	ret = sysfs_create_group(kobj, &group);
	if (ret)
		goto put;
	INIT_DELAYED_WORK(&work, sample_work);
	register_pm_notifier(&pm_nb);
	if (sampling)
		queue_delayed_work(system_freezable_power_efficient_wq, &work, 0);
	pr_info("zrn-therm: %s, gauge writes %s\n", sampling ? "sampling" : "on request only",
		write_gauge ? "on" : "off");
	return 0;
put:
	kobject_put(kobj);
unmap:
	if (adc)
		iounmap(adc);
	if (gates)
		iounmap(gates);
	return ret;
}

static void __exit zrn_therm_exit(void)
{
	int i;

	sampling = false;
	unregister_pm_notifier(&pm_nb);
	cancel_delayed_work_sync(&work);
	sysfs_remove_group(kobj, &group);
	kobject_put(kobj);
	for (i = 0; i < 2; i++) {
		if (st.gate_opened[i]) {
			u32 v = readl(gates + 4 * i);

			writel(v & ~GATE_CG_VAL, gates + 4 * i);
			pr_info("zrn-therm: ADC clock gate %d closed again: 0x%08x -> 0x%08x\n",
				i, v, readl(gates + 4 * i));
		}
	}
	iounmap(adc);
	iounmap(gates);
}

module_init(zrn_therm_init);
module_exit(zrn_therm_exit);
MODULE_DESCRIPTION("S9 thermistors on the Exynos 9810 ADC, and the fuel gauge's Temp");
MODULE_LICENSE("GPL");
