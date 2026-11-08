#!/usr/bin/env python3
"""MAX77705 charger: stock-like AICL handling (and more, see below).

  patch-max77705-charger.py <linux tree>

Patches the port's copy of drivers/power/supply/max77705_charger.c (the one
apply_drivers.sh installs, with charge_behaviour) and the private state in
include/linux/power/max77705_charger.h. Idempotent; every anchor is checked.

AICL. Mainline's handler looped inside the threaded IRQ while INT_OK.AICL_OK
was clear, lowering CHGIN_ILIM one code (25 mA) per 100 ms with no floor:
code 0 minus one is 0x7F in the 7-bit field, 3.2 A. Stock
(max77705_aicl_isr_work, reduce_input_current) masks the interrupt, works in
a delayed work, steps 100 mA to a 300 mA floor and reports the result. This
does the same: the interrupt masks itself and queues the work; each pass (100
ms apart) lowers the active input's limit by 100 mA, never below 300 mA, and
unmasks once AICL_OK is back. At the floor it stays masked until the input
changes, as stock disables AICL there. The limit AICL left is readable in
input_current_limit_aicl (uA, 0 = no reduction since the input appeared), and
a write of input_current_limit above it is clamped to it until the input
goes away, so userspace cannot push the limit back into the collapse.

The input limits are written as code = mA / 25 - 1, the inverse of the
(code + 1) * 25 mA read-back (stock's formula); mainline wrote mA / 25, so
1500 mA read back as 1525 mA.

Charge current. Mainline never applied the monitored battery's
constant-charge-current-max, so CNFG_02 stayed at its 450 mA default; it is
programmed at probe now (1500 mA in ztOS's DT, under stock's
battery,max_charging_current of 2150 mA). Writes are clamped to 3150 mA, the
largest code of the 6-bit field: mainline's 3200 mA clamp gave code 64, which
the field truncated to 0, 100 mA.
"""
from pathlib import Path
import sys

TREE = Path(sys.argv[1])
CFILE = TREE / 'drivers/power/supply/max77705_charger.c'
HFILE = TREE / 'include/linux/power/max77705_charger.h'
MARK = 'zboot: max77705 AICL'


def die(what):
    sys.exit(f'patch-max77705-charger: {what}')


# --- header: driver state, between markers so a newer block replaces an older one
h = HFILE.read_text()
H_BEGIN = '\t/* zboot: max77705-charger state { */\n'
H_END = '\t/* } zboot */\n'
H_BLOCK = H_BEGIN + '''\tstruct mutex		lock;		/* limits, AICL and input state */
\tstruct delayed_work	aicl_work;
\tint			aicl_irq;
\tbool			aicl_masked;	/* aicl_irq disabled by its handler */
\tint			chgin_on;	/* -1 until the first input check */
\tint			aicl_ua[2];	/* limit AICL left, per input; 0 = none */
''' + H_END
if H_BEGIN in h:
    a = h.index(H_BEGIN)
    b = h.index(H_END, a) + len(H_END)
    h = h[:a] + H_BLOCK + h[b:]
else:
    anchor = '\tstruct power_supply\t*psy_chg;\n};'
    if h.count(anchor) != 1:
        die('header struct anchor changed')
    h = h.replace(anchor, '\tstruct power_supply\t*psy_chg;\n' + H_BLOCK + '};')
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

# --- the AICL interrupt, its work, and the input-limit helpers
region('static irqreturn_t max77705_aicl_irq(int irq, void *irq_drv_data)\n',
       'static irqreturn_t max77705_chgin_irq(int irq, void *irq_drv_data)\n', r'''/*
 * zboot: max77705 AICL, done the stock way (max77705_aicl_isr_work and
 * reduce_input_current in Samsung's driver): masked interrupt, delayed work,
 * 100 mA steps, 300 mA floor.
 */
#define MAX77705_AICL_STEP_UA		100000	/* stock REDUCE_CURRENT_STEP */
#define MAX77705_AICL_FLOOR_UA		300000	/* stock MINIMUM_INPUT_CURRENT */

enum max77705_input {
	MAX77705_IN_CHGIN,
	MAX77705_IN_WCIN,
};

/* CHGIN_ILIM (CNFG_09) and WCIN_ILIM (CNFG_10): (code + 1) * 25 mA, 0-3 = 100 mA */
static const struct {
	enum max77705_field_idx field;
	int max_ua;
	const char *name;
} max77705_ilim[] = {
	[MAX77705_IN_CHGIN] = { MAX77705_CHG_CHGIN_LIM, MAX77705_CURRENT_CHGIN_MAX, "CHGIN" },
	[MAX77705_IN_WCIN] = { MAX77705_CHG_WCIN, 1600000, "WCIN" },
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

/* The limit is rounded down to the 25 mA step; returns what was programmed. */
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

/* chg->lock held */
static void max77705_aicl_unmask(struct max77705_charger_data *chg)
{
	if (chg->aicl_masked) {
		chg->aicl_masked = false;
		enable_irq(chg->aicl_irq);
	}
}

static void max77705_aicl_work(struct work_struct *work)
{
	struct max77705_charger_data *chg =
		container_of(to_delayed_work(work), struct max77705_charger_data,
			     aicl_work);
	enum max77705_input in;
	unsigned int ok;
	int cur, new;
	bool changed = false;

	mutex_lock(&chg->lock);

	if (regmap_read(chg->regmap, MAX77705_CHG_REG_INT_OK, &ok))
		goto unmask;
	/* AICL_OK set: the input is out of regulation again */
	if (ok & MAX77705_AICL_OK)
		goto unmask;
	if (ok & MAX77705_CHGIN_OK)
		in = MAX77705_IN_CHGIN;
	else
		goto unmask;

	if (max77705_ilim_read(chg, in, &cur))
		goto unmask;
	if (cur <= MAX77705_AICL_FLOOR_UA) {
		/* as stock: no lower, and no more interrupts until the input changes */
		dev_info(chg->dev, "AICL: %s limit at the %d mA floor, AICL masked\n",
			 max77705_ilim[in].name, cur / 1000);
		goto out;
	}

	new = max(cur - MAX77705_AICL_STEP_UA, MAX77705_AICL_FLOOR_UA);
	if (max77705_ilim_write(chg, in, new))
		goto unmask;
	chg->aicl_ua[in] = max77705_ilim_round(in, new);
	changed = true;
	dev_info(chg->dev, "AICL: %s limit %d -> %d mA\n", max77705_ilim[in].name,
		 cur / 1000, chg->aicl_ua[in] / 1000);

	/* still masked; look again in 100 ms */
	queue_delayed_work(chg->wqueue, &chg->aicl_work,
			   msecs_to_jiffies(AICL_WORK_DELAY_MS));
	goto out;

unmask:
	max77705_aicl_unmask(chg);
out:
	mutex_unlock(&chg->lock);
	if (changed)
		power_supply_changed(chg->psy_chg);
}

static irqreturn_t max77705_aicl_irq(int irq, void *irq_drv_data)
{
	struct max77705_charger_data *chg = irq_drv_data;

	/* masked until the work has finished stepping down */
	disable_irq_nosync(irq);
	mutex_lock(&chg->lock);
	chg->aicl_masked = true;
	mod_delayed_work(chg->wqueue, &chg->aicl_work,
			 msecs_to_jiffies(AICL_WORK_DELAY_MS));
	mutex_unlock(&chg->lock);

	return IRQ_HANDLED;
}

static ssize_t input_current_limit_aicl_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct power_supply *psy = dev_to_psy(dev);
	struct max77705_charger_data *chg = power_supply_get_drvdata(psy);
	enum max77705_input in = MAX77705_IN_CHGIN;

	return sysfs_emit(buf, "%d\n", READ_ONCE(chg->aicl_ua[in]));
}
static DEVICE_ATTR_RO(input_current_limit_aicl);

static struct attribute *max77705_charger_attrs[] = {
	&dev_attr_input_current_limit_aicl.attr,
	NULL
};
ATTRIBUTE_GROUPS(max77705_charger);

''')

# --- input limit read-back through the helper
region('static int max77705_get_input_current(struct max77705_charger_data *chg,\n',
       'static int max77705_get_charge_current(', '''static int max77705_get_input_current(struct max77705_charger_data *chg,
					int *val)
{
	return max77705_ilim_read(chg, MAX77705_IN_CHGIN, val);
}

''')

# --- input limit writes: stock's formula, and never above what AICL left
rep('''	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		err = max77705_set_integer(chg, MAX77705_CHG_CHGIN_LIM,
					   MAX77705_CURRENT_CHGIN_MIN,
					   MAX77705_CURRENT_CHGIN_MAX,
					   MAX77705_CURRENT_CHGIN_STEP,
					   val->intval);
		break;
''', '''	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		mutex_lock(&chg->lock);
		limit = val->intval;
		if (chg->aicl_ua[MAX77705_IN_CHGIN] &&
		    limit > chg->aicl_ua[MAX77705_IN_CHGIN])
			limit = chg->aicl_ua[MAX77705_IN_CHGIN];
		err = max77705_ilim_write(chg, MAX77705_IN_CHGIN, limit);
		mutex_unlock(&chg->lock);
		break;
''')
rep('''	struct max77705_charger_data *chg = power_supply_get_drvdata(psy);
	int err = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:''', '''	struct max77705_charger_data *chg = power_supply_get_drvdata(psy);
	int err = 0, limit;

	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:''')

# --- CHGIN changes: a new input starts without an AICL reduction
region('static void max77705_chgin_isr_work(struct work_struct *work)\n',
       'static int max77705_charger_initialize(', '''static void max77705_chgin_isr_work(struct work_struct *work)
{
	struct max77705_charger_data *chg =
		container_of(work, struct max77705_charger_data, chgin_work);
	unsigned int ok;
	int chgin;

	mutex_lock(&chg->lock);
	if (!regmap_read(chg->regmap, MAX77705_CHG_REG_INT_OK, &ok)) {
		chgin = !!(ok & MAX77705_CHGIN_OK);
		if (chgin != chg->chgin_on) {
			chg->chgin_on = chgin;
			chg->aicl_ua[MAX77705_IN_CHGIN] = 0;
			/* a floor-masked AICL listens again for the new input */
			cancel_delayed_work(&chg->aicl_work);
			max77705_aicl_unmask(chg);
		}
	}
	mutex_unlock(&chg->lock);

	power_supply_changed(chg->psy_chg);
}

''')

# --- probe
rep('''	pscfg.fwnode = dev_fwnode(dev);
	pscfg.drv_data = chg;
''', '''	mutex_init(&chg->lock);
	chg->chgin_on = -1;

	pscfg.fwnode = dev_fwnode(dev);
	pscfg.drv_data = chg;
	pscfg.attr_grp = max77705_charger_groups;
''')
rep('''		return dev_err_probe(dev, ret, "failed to initialize interrupt work\\n");
''', '''		return dev_err_probe(dev, ret, "failed to initialize interrupt work\\n");

	ret = devm_delayed_work_autocancel(dev, &chg->aicl_work, max77705_aicl_work);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize AICL work\\n");
''')
rep('''	ret = devm_request_threaded_irq(dev, regmap_irq_get_virq(irq_data, MAX77705_AICL_I),
					NULL, max77705_aicl_irq,''', '''	chg->aicl_irq = regmap_irq_get_virq(irq_data, MAX77705_AICL_I);
	ret = devm_request_threaded_irq(dev, chg->aicl_irq,
					NULL, max77705_aicl_irq,''')

# --- charge current: the battery's maximum at probe, and a clamp that fits the field
rep('static const struct regmap_config max77705_chg_regmap_config = {', '''/* zboot: CNFG_02 CHG_CC is 6 bits of 50 mA: 63 = 3150 mA */
#define MAX77705_CURRENT_CC_MAX		3150000

static const struct regmap_config max77705_chg_regmap_config = {''')
rep('''	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		err = max77705_set_integer(chg, MAX77705_CHG_CC_LIM,
					   MAX77705_CURRENT_CHGIN_MIN,
					   MAX77705_CURRENT_CHGIN_MAX,
''', '''	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		err = max77705_set_integer(chg, MAX77705_CHG_CC_LIM,
					   MAX77705_CURRENT_CHGIN_MIN,
					   MAX77705_CURRENT_CC_MAX,
''')
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

CFILE.write_text(s)
print('patch-max77705-charger: AICL work, 100 mA steps to a 300 mA floor; '
      'charge current from the battery node')
