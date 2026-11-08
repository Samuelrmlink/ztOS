#!/usr/bin/env python3
"""MAX77705 charger: AICL, input-current ramp, battery charge current, WCIN.

  patch-max77705-charger.py <linux tree>

Patches the port's copy of drivers/power/supply/max77705_charger.c (the one
apply_drivers.sh installs, with charge_behaviour) and the private state in
include/linux/power/max77705_charger.h. Idempotent; every anchor is checked.

Input current, both inputs (CHGIN = USB, WCIN = wireless receiver):

* input_current_limit is a ceiling. The kernel starts each plug-in, each
  ceiling raise and each change of input voltage (the USB-C supply's
  voltage_now moving by 1 V or more, 5 V <-> 9 V) from a safe 500 mA, or lower
  if the limit already is lower, and steps it up 100 mA at a time (CHGIN every
  250 ms, WCIN every 1000 ms as stock's wc_current_work) while the input holds
  up. A lower ceiling applies at once; rewriting the same ceiling changes
  nothing. The ceiling goes back to its default when the input goes away
  (CHGIN 500 mA; WCIN 900 mA, stock's cable-info WIRELESS), and the limit
  rests at CHGIN 500 mA / WCIN 425 mA (stock parks CNFG_10 at 0x10).
* "Holds up" is INT_OK.AICL_OK: the charger's own comparator for VBUS at
  VCHGIN_REG (4.5 V, stock's setting). There is no cheaper or finer VBUS
  reading from this driver: VBYP sits in the fuel gauge at another I2C
  address and driver, the USB-C VBADC is 1 V bands, the hwmon driver is off.
* AICL (VBUS sagging to 4.5 V) is handled the stock way (max77705_aicl_isr_work,
  reduce_input_current): the interrupt masks itself and a delayed work backs
  the limit off 200 mA, then 100 mA every 100 ms while AICL stays active, never
  below 300 mA (stock MINIMUM_INPUT_CURRENT), never wrapping. The result is
  the "found" limit, held until the input goes away or the ceiling is raised;
  stock likewise keeps aicl_current until the cable changes and never retries
  above it. At the floor AICL stays masked until the input changes. One log
  line per back-off, one uevent when it settles.
  Mainline instead looped inside the threaded IRQ lowering CHGIN_ILIM 25 mA per
  100 ms with no floor, so code 0 minus one became 0x7F, 3.2 A.
* Read-only, on max77705-charger and max77705-wireless:
  input_current_limit_effective (uA, the register now), input_current_limit_found
  (uA, 0 = no back-off this plug-in), input_current_ramp (idle, ramping,
  holding, backed-off).
* Limits are written as code = mA / 25 - 1, the inverse of the (code + 1) * 25
  read-back (stock's formula); mainline wrote mA / 25, so 1500 read as 1525.

Charge current: the monitored battery's constant-charge-current-max is
programmed at probe (1500 mA in ztOS's DT, under stock's max_charging_current
2150 mA) instead of staying at the 450 mA default. Writes are clamped to
3150 mA, the 6-bit field's top: the old 3200 mA clamp gave code 64, which the
field truncated to 0 (100 mA).

Wireless: the IDT P9320's output feeds WCIN, which mainline ignored. A second
power_supply, max77705-wireless (type Wireless), reports online from
INT_OK.WCIN_OK, follows the WCIN interrupt (WCIN_I of the driver's own regmap
IRQ chip) and has the WCIN ceiling (CNFG_10, 25 mA steps, at most 1600 mA)
as input_current_limit. On this board WCIN_OK and WCIN_DTLS read valid
whenever VBUS is present (INT_OK 0xff, DETAILS_00 0xf8 on a USB charger), so
WCIN counts as the wireless input only while CHGIN is not valid; CHGIN wins
when both are there anyway. The wireless supply takes the charger's
"wireless" child node when the DT has one, so the fuel gauge can list it in
power-supplies and report Charging on a pad.
"""
from pathlib import Path
import sys

TREE = Path(sys.argv[1])
CFILE = TREE / 'drivers/power/supply/max77705_charger.c'
HFILE = TREE / 'include/linux/power/max77705_charger.h'
MARK = 'zboot: max77705 AICL'


def die(what):
    sys.exit(f'patch-max77705-charger: {what}')


# --- header: driver state, in marked blocks so a newer block replaces an older one
h = HFILE.read_text()


def hblock(begin, end, body, anchor, before):
    global h
    block = begin + body + end
    if begin in h:
        a = h.index(begin)
        b = h.index(end, a) + len(end)
        h = h[:a] + block + h[b:]
        return
    if h.count(anchor) != 1:
        die(f'header anchor changed: {anchor!r}')
    h = h.replace(anchor, block + anchor if before else anchor + block)


hblock('/* zboot: max77705 input state { */\n', '/* } zboot input state */\n\n', '''enum max77705_input {
	MAX77705_IN_CHGIN,
	MAX77705_IN_WCIN,
	MAX77705_N_INPUTS,
};

struct max77705_charger_data;

struct max77705_input_state {
	struct max77705_charger_data *chg;
	enum max77705_input	index;
	struct delayed_work	ramp_work;
	int			online;		/* -1 until the first input check */
	int			ceil_ua;	/* input_current_limit */
	int			found_ua;	/* AICL back-off result; 0 = none */
	int			ramp;		/* enum max77705_ramp */
	int			backoff_from_ua; /* limit before a back-off in progress */
};
''', 'struct max77705_charger_data {\n', before=True)
hblock('\t/* zboot: max77705-charger state { */\n', '\t/* } zboot */\n', '''\tstruct mutex		lock;		/* limits, ramps, AICL */
\tstruct delayed_work	aicl_work;
\tint			aicl_irq;
\tbool			aicl_masked;	/* aicl_irq disabled by its handler */
\tstruct max77705_input_state in[MAX77705_N_INPUTS];
\tstruct power_supply	*psy_wc;
\tint			vbus_uv;	/* USB-C supply voltage_now; 0 = unknown */
''', '\tstruct power_supply\t*psy_chg;\n', before=False)
HFILE.write_text(h)

s = CFILE.read_text()
if MARK in s:
    print('patch-max77705-charger: already applied')
    sys.exit(0)


def rep(old, new):
    global s
    if s.count(old) != 1:
        die(f'anchor changed: {old.splitlines()[0]!r}')
    s = s.replace(old, new)


def region(start, end, new):
    """Replace from the start anchor up to (not including) the end anchor."""
    global s
    if s.count(start) != 1:
        die(f'anchor changed: {start!r}')
    a = s.index(start)
    b = s.find(end, a)
    if b < 0:
        die(f'anchor changed: {end!r}')
    s = s[:a] + new + s[b:]


rep('#include <linux/module.h>\n', '#include <linux/module.h>\n#include <linux/mutex.h>\n')
rep('static const struct regmap_config max77705_chg_regmap_config = {', '''/* zboot: CNFG_02 CHG_CC is 6 bits of 50 mA: 63 = 3150 mA */
#define MAX77705_CURRENT_CC_MAX		3150000

static const struct regmap_config max77705_chg_regmap_config = {''')

# --- AICL, the ramp, and the input-limit helpers
region('static irqreturn_t max77705_aicl_irq(int irq, void *irq_drv_data)\n',
       'static irqreturn_t max77705_chgin_irq(int irq, void *irq_drv_data)\n', r'''/*
 * zboot: max77705 AICL and input-current ramp (see kernel/patch-max77705-charger.py
 * in ztOS). input_current_limit is a ceiling; the limit ramps up to it from
 * 500 mA while INT_OK.AICL_OK says VBUS holds above VCHGIN_REG, and an AICL
 * event backs it off and holds it there, as stock's max77705_aicl_isr_work.
 */
#define MAX77705_AICL_FLOOR_UA		300000	/* stock MINIMUM_INPUT_CURRENT */
#define MAX77705_BACKOFF_UA		200000	/* first step of a back-off */
#define MAX77705_AICL_STEP_UA		100000	/* stock REDUCE_CURRENT_STEP */
#define MAX77705_RAMP_STEP_UA		100000	/* stock WC_CURRENT_STEP */
#define MAX77705_SAFE_START_UA		500000
#define MAX77705_VBUS_CHANGE_UV		1000000

enum max77705_ramp {
	MAX77705_RAMP_IDLE,
	MAX77705_RAMP_RAMPING,
	MAX77705_RAMP_HOLDING,
	MAX77705_RAMP_BACKED_OFF,
};

static const char * const max77705_ramp_name[] = {
	[MAX77705_RAMP_IDLE]		= "idle",
	[MAX77705_RAMP_RAMPING]		= "ramping",
	[MAX77705_RAMP_HOLDING]		= "holding",
	[MAX77705_RAMP_BACKED_OFF]	= "backed-off",
};

/* CHGIN_ILIM (CNFG_09) and WCIN_ILIM (CNFG_10): (code + 1) * 25 mA, 0-3 = 100 mA */
static const struct {
	enum max77705_field_idx field;
	int max_ua;
	int default_ceil_ua;	/* ceiling at each new plug-in */
	int idle_ua;		/* limit while the input is absent */
	unsigned int step_ms;	/* ramp pace */
	const char *name;
} max77705_ilim[] = {
	[MAX77705_IN_CHGIN] = { MAX77705_CHG_CHGIN_LIM, MAX77705_CURRENT_CHGIN_MAX,
				500000, 500000, 250, "CHGIN" },
	/* stock: cable-info WIRELESS 900 mA, CNFG_10 = 0x10 when idle, 100 mA/s */
	[MAX77705_IN_WCIN] = { MAX77705_CHG_WCIN, 1600000,
			       900000, 425000, 1000, "WCIN" },
};

static int max77705_ilim_read(struct max77705_charger_data *chg,
			      enum max77705_input in, int *ua)
{
	unsigned int code;
	int ret;

	ret = regmap_field_read(chg->rfield[max77705_ilim[in].field], &code);
	if (ret)
		return ret;

	*ua = code <= 3 ? MAX77705_CURRENT_CHGIN_MIN :
			  (code + 1) * MAX77705_CURRENT_CHGIN_STEP;
	return 0;
}

/* what max77705_ilim_write() programs for @ua */
static int max77705_ilim_round(enum max77705_input in, int ua)
{
	ua = clamp(ua, MAX77705_CURRENT_CHGIN_MIN, max77705_ilim[in].max_ua);
	return ua - ua % MAX77705_CURRENT_CHGIN_STEP;
}

static int max77705_ilim_write(struct max77705_charger_data *chg,
			       enum max77705_input in, int ua)
{
	ua = max77705_ilim_round(in, ua);

	return regmap_field_write(chg->rfield[max77705_ilim[in].field],
				  ua / MAX77705_CURRENT_CHGIN_STEP - 1);
}

/*
 * WCIN reads valid whenever VBUS is present on this board, so it is the
 * wireless input only while CHGIN is not valid (CHGIN wins anyway).
 */
static bool max77705_wc_valid(unsigned int int_ok)
{
	return (int_ok & MAX77705_WCIN_OK) && !(int_ok & MAX77705_CHGIN_OK);
}

static struct power_supply *max77705_psy(struct max77705_charger_data *chg,
					 enum max77705_input in)
{
	return in == MAX77705_IN_WCIN ? chg->psy_wc : chg->psy_chg;
}

/* All below with chg->lock held. */

static void max77705_aicl_unmask(struct max77705_charger_data *chg)
{
	if (chg->aicl_masked) {
		chg->aicl_masked = false;
		enable_irq(chg->aicl_irq);
	}
}

static int max77705_ramp_target(struct max77705_input_state *st)
{
	return st->found_ua ? min(st->found_ua, st->ceil_ua) : st->ceil_ua;
}

static void max77705_ramp_settle(struct max77705_input_state *st)
{
	st->ramp = st->found_ua && st->found_ua < st->ceil_ua ?
		   MAX77705_RAMP_BACKED_OFF : MAX77705_RAMP_HOLDING;
}

/* from the safe start (or lower) up to the ceiling */
static void max77705_ramp_restart(struct max77705_charger_data *chg,
				  enum max77705_input in)
{
	struct max77705_input_state *st = &chg->in[in];
	int cur, start;

	cancel_delayed_work(&st->ramp_work);
	if (max77705_ilim_read(chg, in, &cur))
		return;
	start = min3(cur, MAX77705_SAFE_START_UA, st->ceil_ua);
	if (start != cur && max77705_ilim_write(chg, in, start))
		return;
	st->ramp = MAX77705_RAMP_RAMPING;
	queue_delayed_work(chg->wqueue, &st->ramp_work,
			   msecs_to_jiffies(max77705_ilim[in].step_ms));
}

/* AICL active on @in: lower the limit one step; false at the floor */
static bool max77705_aicl_step(struct max77705_charger_data *chg,
			       enum max77705_input in, int cur)
{
	struct max77705_input_state *st = &chg->in[in];
	bool first = !st->backoff_from_ua;
	int new;

	cancel_delayed_work(&st->ramp_work);
	st->ramp = MAX77705_RAMP_BACKED_OFF;
	if (first)
		st->backoff_from_ua = cur;
	if (cur <= MAX77705_AICL_FLOOR_UA) {
		st->found_ua = cur;
		return false;
	}

	new = cur - (first ? MAX77705_BACKOFF_UA : MAX77705_AICL_STEP_UA);
	new = max77705_ilim_round(in, max(new, MAX77705_AICL_FLOOR_UA));
	if (max77705_ilim_write(chg, in, new))
		return false;
	st->found_ua = new;
	return true;
}

/* a back-off has settled: one line for all its steps */
static bool max77705_aicl_done(struct max77705_charger_data *chg,
			       enum max77705_input in)
{
	struct max77705_input_state *st = &chg->in[in];

	if (!st->backoff_from_ua)
		return false;
	dev_info(chg->dev, "%s sagged (AICL): input limit %d -> %d mA, held (ceiling %d mA)\n",
		 max77705_ilim[in].name, st->backoff_from_ua / 1000,
		 st->found_ua / 1000, st->ceil_ua / 1000);
	st->backoff_from_ua = 0;
	return true;
}

static void max77705_aicl_work(struct work_struct *work)
{
	struct max77705_charger_data *chg =
		container_of(to_delayed_work(work), struct max77705_charger_data,
			     aicl_work);
	enum max77705_input in = MAX77705_IN_CHGIN;
	unsigned int ok;
	bool notify = false;
	int cur;

	mutex_lock(&chg->lock);

	if (regmap_read(chg->regmap, MAX77705_CHG_REG_INT_OK, &ok))
		goto unmask;
	if (ok & MAX77705_CHGIN_OK)
		in = MAX77705_IN_CHGIN;
	else if (max77705_wc_valid(ok))
		in = MAX77705_IN_WCIN;
	else
		goto unmask;

	/* AICL_OK set: VBUS is above VCHGIN_REG again */
	if (ok & MAX77705_AICL_OK) {
		notify = max77705_aicl_done(chg, in);
		goto unmask;
	}

	if (max77705_ilim_read(chg, in, &cur))
		goto unmask;
	if (max77705_aicl_step(chg, in, cur)) {
		/* still masked; look again in 100 ms */
		queue_delayed_work(chg->wqueue, &chg->aicl_work,
				   msecs_to_jiffies(AICL_WORK_DELAY_MS));
		goto out;
	}

	/* as stock: at the floor, no more AICL interrupts until the input changes */
	notify = max77705_aicl_done(chg, in);
	dev_info(chg->dev, "AICL: %s at the %d mA floor, AICL masked until the input changes\n",
		 max77705_ilim[in].name, chg->in[in].found_ua / 1000);
	goto out;

unmask:
	max77705_aicl_unmask(chg);
out:
	mutex_unlock(&chg->lock);
	if (notify)
		power_supply_changed(max77705_psy(chg, in));
}

static irqreturn_t max77705_aicl_irq(int irq, void *irq_drv_data)
{
	struct max77705_charger_data *chg = irq_drv_data;

	/* masked until the work has finished backing off */
	disable_irq_nosync(irq);
	mutex_lock(&chg->lock);
	chg->aicl_masked = true;
	mod_delayed_work(chg->wqueue, &chg->aicl_work,
			 msecs_to_jiffies(AICL_WORK_DELAY_MS));
	mutex_unlock(&chg->lock);

	return IRQ_HANDLED;
}

static void max77705_ramp_work(struct work_struct *work)
{
	struct max77705_input_state *st =
		container_of(to_delayed_work(work), struct max77705_input_state,
			     ramp_work);
	struct max77705_charger_data *chg = st->chg;
	enum max77705_input in = st->index;
	bool settled = false;
	unsigned int ok;
	int cur, target, new;

	mutex_lock(&chg->lock);
	if (st->online != 1 || st->ramp != MAX77705_RAMP_RAMPING)
		goto out;
	if (regmap_read(chg->regmap, MAX77705_CHG_REG_INT_OK, &ok) ||
	    max77705_ilim_read(chg, in, &cur))
		goto out;

	if (!(ok & MAX77705_AICL_OK)) {
		/* the input sags already: back off; the AICL work follows it up */
		max77705_aicl_step(chg, in, cur);
		mod_delayed_work(chg->wqueue, &chg->aicl_work,
				 msecs_to_jiffies(AICL_WORK_DELAY_MS));
		goto out;
	}

	target = max77705_ramp_target(st);
	new = min(cur + MAX77705_RAMP_STEP_UA, target);
	if (new != cur && max77705_ilim_write(chg, in, new))
		goto out;
	if (new >= target) {
		max77705_ramp_settle(st);
		settled = true;
	} else {
		queue_delayed_work(chg->wqueue, &st->ramp_work,
				   msecs_to_jiffies(max77705_ilim[in].step_ms));
	}
out:
	mutex_unlock(&chg->lock);
	if (settled)
		power_supply_changed(max77705_psy(chg, in));
}

/* input_current_limit: the ceiling. Lower at once, higher through the ramp. */
static void max77705_set_ceiling(struct max77705_charger_data *chg,
				 enum max77705_input in, int ua)
{
	struct max77705_input_state *st = &chg->in[in];
	bool raise;
	int cur;

	ua = max77705_ilim_round(in, ua);
	if (ua == st->ceil_ua)
		return;		/* rewriting the same ceiling changes nothing */
	raise = ua > st->ceil_ua;
	st->ceil_ua = ua;
	if (st->online != 1)
		return;		/* takes effect at the next plug-in */

	if (raise) {
		st->found_ua = 0;
		max77705_aicl_unmask(chg);	/* if it was left masked at the floor */
		max77705_ramp_restart(chg, in);
		return;
	}
	if (!max77705_ilim_read(chg, in, &cur) && cur > ua)
		max77705_ilim_write(chg, in, ua);
	if (st->ramp == MAX77705_RAMP_HOLDING ||
	    (st->ramp == MAX77705_RAMP_BACKED_OFF && !st->backoff_from_ua))
		max77705_ramp_settle(st);
}

static enum max77705_input max77705_attr_input(struct device *dev,
						struct max77705_charger_data **chg)
{
	struct power_supply *psy = dev_to_psy(dev);

	*chg = power_supply_get_drvdata(psy);
	return psy->desc->type == POWER_SUPPLY_TYPE_WIRELESS ?
	       MAX77705_IN_WCIN : MAX77705_IN_CHGIN;
}

static ssize_t input_current_limit_effective_show(struct device *dev,
						  struct device_attribute *attr,
						  char *buf)
{
	struct max77705_charger_data *chg;
	enum max77705_input in = max77705_attr_input(dev, &chg);
	int ua, ret;

	ret = max77705_ilim_read(chg, in, &ua);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%d\n", ua);
}
static DEVICE_ATTR_RO(input_current_limit_effective);

static ssize_t input_current_limit_found_show(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct max77705_charger_data *chg;
	enum max77705_input in = max77705_attr_input(dev, &chg);

	return sysfs_emit(buf, "%d\n", READ_ONCE(chg->in[in].found_ua));
}
static DEVICE_ATTR_RO(input_current_limit_found);

static ssize_t input_current_ramp_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct max77705_charger_data *chg;
	enum max77705_input in = max77705_attr_input(dev, &chg);

	return sysfs_emit(buf, "%s\n",
			  max77705_ramp_name[READ_ONCE(chg->in[in].ramp)]);
}
static DEVICE_ATTR_RO(input_current_ramp);

static struct attribute *max77705_charger_attrs[] = {
	&dev_attr_input_current_limit_effective.attr,
	&dev_attr_input_current_limit_found.attr,
	&dev_attr_input_current_ramp.attr,
	NULL
};
ATTRIBUTE_GROUPS(max77705_charger);

''')

# --- input_current_limit reads the ceiling
region('static int max77705_get_input_current(struct max77705_charger_data *chg,\n',
       'static int max77705_get_charge_current(', '''static int max77705_get_input_current(struct max77705_charger_data *chg,
					int *val)
{
	/* the ceiling; input_current_limit_effective has the register */
	*val = READ_ONCE(chg->in[MAX77705_IN_CHGIN].ceil_ua);
	return 0;
}

''')

# --- writes: ceiling for the input limit, a field-sized clamp for the charge current
rep('''	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		err = max77705_set_integer(chg, MAX77705_CHG_CC_LIM,
					   MAX77705_CURRENT_CHGIN_MIN,
					   MAX77705_CURRENT_CHGIN_MAX,
					   MAX77705_CURRENT_CHG_STEP,
					   val->intval);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		err = max77705_set_integer(chg, MAX77705_CHG_CHGIN_LIM,
					   MAX77705_CURRENT_CHGIN_MIN,
					   MAX77705_CURRENT_CHGIN_MAX,
					   MAX77705_CURRENT_CHGIN_STEP,
					   val->intval);
		break;
''', '''	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		err = max77705_set_integer(chg, MAX77705_CHG_CC_LIM,
					   MAX77705_CURRENT_CHGIN_MIN,
					   MAX77705_CURRENT_CC_MAX,
					   MAX77705_CURRENT_CHG_STEP,
					   val->intval);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		mutex_lock(&chg->lock);
		max77705_set_ceiling(chg, MAX77705_IN_CHGIN, val->intval);
		mutex_unlock(&chg->lock);
		break;
''')

# --- the wireless supply, and a supplier change (USB-C voltage) rechecks the inputs
rep('''static const struct power_supply_desc max77705_charger_psy_desc = {
	.name = "max77705-charger",''', '''static void max77705_external_power_changed(struct power_supply *psy)
{
	struct max77705_charger_data *chg = power_supply_get_drvdata(psy);

	queue_work(chg->wqueue, &chg->chgin_work);
}

static enum power_supply_property max77705_wc_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
};

static int max77705_wc_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct max77705_charger_data *chg = power_supply_get_drvdata(psy);
	unsigned int ok;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		ret = regmap_read(chg->regmap, MAX77705_CHG_REG_INT_OK, &ok);
		if (ret)
			return ret;
		val->intval = max77705_wc_valid(ok);
		return 0;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		val->intval = READ_ONCE(chg->in[MAX77705_IN_WCIN].ceil_ua);
		return 0;
	default:
		return -EINVAL;
	}
}

static int max77705_wc_set_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    const union power_supply_propval *val)
{
	struct max77705_charger_data *chg = power_supply_get_drvdata(psy);

	if (psp != POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT)
		return -EINVAL;
	mutex_lock(&chg->lock);
	max77705_set_ceiling(chg, MAX77705_IN_WCIN, val->intval);
	mutex_unlock(&chg->lock);
	return 0;
}

static int max77705_wc_property_is_writeable(struct power_supply *psy,
					     enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT;
}

static const struct power_supply_desc max77705_wc_psy_desc = {
	.name = "max77705-wireless",
	.type = POWER_SUPPLY_TYPE_WIRELESS,
	.properties = max77705_wc_props,
	.num_properties = ARRAY_SIZE(max77705_wc_props),
	.get_property = max77705_wc_get_property,
	.set_property = max77705_wc_set_property,
	.property_is_writeable = max77705_wc_property_is_writeable,
};

static const struct power_supply_desc max77705_charger_psy_desc = {
	.name = "max77705-charger",
	.external_power_changed = max77705_external_power_changed,''')

# --- input changes: CHGIN and WCIN interrupts, and the USB-C supply's voltage
region('static void max77705_chgin_isr_work(struct work_struct *work)\n',
       'static int max77705_charger_initialize(', '''static void max77705_chgin_isr_work(struct work_struct *work)
{
	struct max77705_charger_data *chg =
		container_of(work, struct max77705_charger_data, chgin_work);
	int on[MAX77705_N_INPUTS], vbus_uv = 0, in;
	union power_supply_propval vbus;
	bool wc_changed = false, input_changed = false;
	struct max77705_input_state *st;
	unsigned int ok;

	/* the source's voltage, from the USB-C supply when the DT lists one */
	if (!power_supply_get_property_from_supplier(chg->psy_chg,
						     POWER_SUPPLY_PROP_VOLTAGE_NOW,
						     &vbus))
		vbus_uv = vbus.intval;

	mutex_lock(&chg->lock);
	if (regmap_read(chg->regmap, MAX77705_CHG_REG_INT_OK, &ok))
		goto out;
	on[MAX77705_IN_CHGIN] = !!(ok & MAX77705_CHGIN_OK);
	on[MAX77705_IN_WCIN] = max77705_wc_valid(ok);

	for (in = 0; in < MAX77705_N_INPUTS; in++) {
		st = &chg->in[in];
		if (on[in] == st->online)
			continue;
		st->online = on[in];
		input_changed = true;
		if (in == MAX77705_IN_WCIN)
			wc_changed = true;
		cancel_delayed_work(&st->ramp_work);
		st->found_ua = 0;
		st->backoff_from_ua = 0;
		if (on[in]) {
			dev_info(chg->dev, "%s valid: input limit ramps up to %d mA\\n",
				 max77705_ilim[in].name, st->ceil_ua / 1000);
			max77705_ramp_restart(chg, in);
		} else {
			st->ceil_ua = max77705_ilim[in].default_ceil_ua;
			st->ramp = MAX77705_RAMP_IDLE;
			max77705_ilim_write(chg, in, max77705_ilim[in].idle_ua);
		}
	}
	if (input_changed) {
		/* a floor-masked AICL listens again for the new input */
		cancel_delayed_work(&chg->aicl_work);
		max77705_aicl_unmask(chg);
	}

	/* 5 V <-> 9 V: a limit found at one voltage says nothing about the other */
	st = &chg->in[MAX77705_IN_CHGIN];
	if (st->online == 1 && vbus_uv && chg->vbus_uv &&
	    abs(vbus_uv - chg->vbus_uv) >= MAX77705_VBUS_CHANGE_UV) {
		dev_info(chg->dev, "input %d -> %d mV: input limit ramps again\\n",
			 chg->vbus_uv / 1000, vbus_uv / 1000);
		st->found_ua = 0;
		st->backoff_from_ua = 0;
		max77705_ramp_restart(chg, MAX77705_IN_CHGIN);
	}
	chg->vbus_uv = st->online == 1 ? vbus_uv : 0;
out:
	mutex_unlock(&chg->lock);

	power_supply_changed(chg->psy_chg);
	if (wc_changed)
		power_supply_changed(chg->psy_wc);
}

''')

# --- charge current: the battery's maximum at probe
rep('''		max77705_set_float_voltage(chg, info->voltage_max_design_uv);
	}
''', '''		max77705_set_float_voltage(chg, info->voltage_max_design_uv);
	}

	/* zboot: the battery's constant-charge-current-max, not the 450 mA default */
	if (info->constant_charge_current_max_ua > 0) {
		err = max77705_set_integer(chg, MAX77705_CHG_CC_LIM,
					   MAX77705_CURRENT_CHGIN_MIN,
					   MAX77705_CURRENT_CC_MAX,
					   MAX77705_CURRENT_CHG_STEP,
					   info->constant_charge_current_max_ua);
		if (err)
			goto err;
	}
''')

# --- probe
region('static int max77705_charger_probe(struct i2c_client *i2c)\n',
       'static const struct of_device_id max77705_charger_of_match[]', '''static void max77705_put_fwnode(void *fwnode)
{
	fwnode_handle_put(fwnode);
}

static int max77705_charger_probe(struct i2c_client *i2c)
{
	struct power_supply_config pscfg = {}, wccfg = {};
	struct device *dev = &i2c->dev;
	struct max77705_charger_data *chg;
	struct regmap_irq_chip *chip_desc;
	struct regmap_irq_chip_data *irq_data;
	struct fwnode_handle *wc_node;
	int ret, i;

	chg = devm_kzalloc(dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;

	chg->dev = dev;
	i2c_set_clientdata(i2c, chg);

	chip_desc = devm_kmemdup(dev, &max77705_charger_irq_chip,
				 sizeof(max77705_charger_irq_chip),
				 GFP_KERNEL);
	if (!chip_desc)
		return -ENOMEM;
	chip_desc->irq_drv_data = chg;

	chg->regmap = devm_regmap_init_i2c(i2c, &max77705_chg_regmap_config);
	if (IS_ERR(chg->regmap))
		return PTR_ERR(chg->regmap);

	for (i = 0; i < MAX77705_N_REGMAP_FIELDS; i++) {
		chg->rfield[i] = devm_regmap_field_alloc(dev, chg->regmap,
							 max77705_reg_field[i]);
		if (IS_ERR(chg->rfield[i]))
			return dev_err_probe(dev, PTR_ERR(chg->rfield[i]),
					     "cannot allocate regmap field\\n");
	}

	ret = devm_mutex_init(dev, &chg->lock);
	if (ret)
		return ret;

	/* works before the supplies: external_power_changed queues one */
	chg->wqueue = devm_alloc_ordered_workqueue(dev, "%s", 0, dev_name(dev));
	if (!chg->wqueue)
		return -ENOMEM;

	ret = devm_work_autocancel(dev, &chg->chgin_work, max77705_chgin_isr_work);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize interrupt work\\n");

	ret = devm_delayed_work_autocancel(dev, &chg->aicl_work, max77705_aicl_work);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize AICL work\\n");

	for (i = 0; i < MAX77705_N_INPUTS; i++) {
		struct max77705_input_state *st = &chg->in[i];

		st->chg = chg;
		st->index = i;
		st->online = -1;
		st->ceil_ua = max77705_ilim[i].default_ceil_ua;
		ret = devm_delayed_work_autocancel(dev, &st->ramp_work,
						   max77705_ramp_work);
		if (ret)
			return dev_err_probe(dev, ret, "failed to initialize ramp work\\n");
	}

	pscfg.fwnode = dev_fwnode(dev);
	pscfg.drv_data = chg;
	pscfg.attr_grp = max77705_charger_groups;

	chg->psy_chg = devm_power_supply_register(dev, &max77705_charger_psy_desc, &pscfg);
	if (IS_ERR(chg->psy_chg))
		return PTR_ERR(chg->psy_chg);

	/* the DT's "wireless" child node, so the fuel gauge can name it a supplier */
	wc_node = device_get_named_child_node(dev, "wireless");
	if (wc_node) {
		ret = devm_add_action_or_reset(dev, max77705_put_fwnode, wc_node);
		if (ret)
			return ret;
	}
	wccfg.fwnode = wc_node;
	wccfg.drv_data = chg;
	wccfg.attr_grp = max77705_charger_groups;

	chg->psy_wc = devm_power_supply_register(dev, &max77705_wc_psy_desc, &wccfg);
	if (IS_ERR(chg->psy_wc))
		return dev_err_probe(dev, PTR_ERR(chg->psy_wc),
				     "failed to register the wireless supply\\n");

	ret = devm_regmap_add_irq_chip(chg->dev, chg->regmap, i2c->irq,
					IRQF_ONESHOT, 0,
					chip_desc, &irq_data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add irq chip\\n");

	ret = max77705_charger_initialize(chg);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize charger IC\\n");

	ret = devm_request_threaded_irq(dev, regmap_irq_get_virq(irq_data, MAX77705_CHGIN_I),
					NULL, max77705_chgin_irq,
					IRQF_TRIGGER_NONE,
					"chgin-irq", chg);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(dev, regmap_irq_get_virq(irq_data, MAX77705_WCIN_I),
					NULL, max77705_chgin_irq,
					IRQF_TRIGGER_NONE,
					"wcin-irq", chg);
	if (ret)
		return ret;

	chg->aicl_irq = regmap_irq_get_virq(irq_data, MAX77705_AICL_I);
	ret = devm_request_threaded_irq(dev, chg->aicl_irq,
					NULL, max77705_aicl_irq,
					IRQF_TRIGGER_NONE,
					"aicl-irq", chg);
	if (ret)
		return ret;

	ret = max77705_charger_enable(chg);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable charge\\n");

	ret = devm_add_action_or_reset(dev, max77705_charger_disable, chg);
	if (ret)
		return ret;

	/* the first look at the inputs: a charger or a pad present at boot */
	queue_work(chg->wqueue, &chg->chgin_work);
	return 0;
}

''')

CFILE.write_text(s)
print('patch-max77705-charger: AICL back-off, input-current ramp, battery charge current, '
      'max77705-wireless')
