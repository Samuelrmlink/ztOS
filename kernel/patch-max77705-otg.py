#!/usr/bin/env python3
"""MAX77705 charger: reverse boost on CHGIN (USB OTG VBUS) as a regulator.

  patch-max77705-otg.py <linux tree>

Run after patch-max77705-charger.py (it anchors on that patch's state).
Idempotent; every anchor is checked.

Mainline max77705_charger has no OTG. This adds a regulator, registered
only when the charger's DT node has a "usb-otg-vbus" child (board wiring:
the USB-C controller names it as its vbus-supply), so a DT without it
gets no OTG and nothing else changes. Its enable/disable are Samsung's
max77705_set_otg() (battery_v2/max77705_charger.c:799-884, S9 tree), minus
the wireless-receiver and CPU-frequency hooks:

  on:  CHGIN interrupt off (stock masks CHGIN_IM, :830-835), BYP interrupt on
       (stock unmasks BYP_IM), OTG_ILIM 900 mA (CNFG_02[7:6] = 1: stock's
       CONFIG_CHARGER_MAX77705_OTG_LIMIT=y value, :941-945; mainline's init
       sets the same), switching frequency 3 MHz (CNFG_08, :838-841), skip
       mode off (CNFG_12 DISKIP, :843-844), then CNFG_00 |= OTG | BOOST
       (0x0a, :846-848).
  off: CNFG_00 &= ~(OTG | BOOST) (:850-852), 50 ms (:854), interrupts back,
       1.5 MHz (:862-865), auto skip (:867-868).

Safety, beyond stock: enable is refused while CHGIN has a valid input
(INT_OK), so the boost is never turned on against a wired supply.

Wireless (WCIN) keeps charging under the boost, as on stock
(max77705_check_wcin_before_otg_on(), :766-797): before the boost the WCIN
limit is held at or below 900 mA (stock wireless_otg_input_current,
sec_battery.c:208-221) and max77705-wireless reports input_voltage_limit
5 V, which the P9320 driver takes as "no 10 V" (stock WPC_VOUT_LOCK,
sec_battery.c:4853-4893); both are released when the boost ends. Under the
boost CHGIN_OK is our own 5 V, so WCIN alone counts as an input then. A
bypass over-current (BYP_I with DETAILS_02 BYP_DTLS bit 0, stock's
max77705_bypass_irq(), :1855-1890) turns the boost off at once and tells the
consumer (REGULATOR_EVENT_OVER_CURRENT). Removal and shutdown turn it off.
While boosting, CHGIN reads as offline (it sees our own 5 V), so no input
ramp, AICL back-off or "online" comes from it; WCIN works as usual.
"""
from pathlib import Path
import sys

TREE = Path(sys.argv[1])
CFILE = TREE / 'drivers/power/supply/max77705_charger.c'
HFILE = TREE / 'include/linux/power/max77705_charger.h'
MARK = 'zboot: OTG boost'


def patch(path, edits):
    s = path.read_text()
    if MARK in s:
        return
    for old, new in edits:
        if s.count(old) != 1:
            sys.exit(f'patch-max77705-otg: {path.name}: expected 1x {old[:70]!r}, '
                     f'found {s.count(old)}')
        s = s.replace(old, new)
    path.write_text(s)


# The header is not reinstalled between builds: one patched by an older
# version of this script lacks the later fields; add them.
_h = HFILE.read_text()
if MARK in _h and 'otg_wc_lock' not in _h:
    _old = '''	bool			otg_on;		/* boost on, under lock */
'''
    if _h.count(_old) != 1:
        sys.exit('patch-max77705-otg: header: otg_on anchor changed')
    HFILE.write_text(_h.replace(_old, _old + '''	bool			otg_wc_lock;	/* WCIN at 5 V, 900 mA: stock WPC_VOUT_LOCK */
'''))

patch(HFILE, [(
    '''	int			trip_vbus_uv;
	/* } zboot */
};''',
    '''	int			trip_vbus_uv;
	/* } zboot */
	/* zboot: OTG boost { */
	struct regulator_dev	*otg_rdev;
	bool			otg_on;		/* boost on, under lock */
	bool			otg_wc_lock;	/* WCIN at 5 V, 900 mA: stock WPC_VOUT_LOCK */
	int			chgin_irq;
	int			byp_irq;
	/* } */
};''')])

OTG_CODE = r'''
/* ---- zboot: OTG boost (USB-C VBUS out) ---------------------------------- */

/*
 * The wireless side of OTG (stock sec_battery CHARGE_OTG_CONTROL on the
 * "wireless" supply, sec_battery.c:4853-4893, and the OTG branch of
 * sec_bat_get_wireless_current(), :208-221): while the boost runs, WCIN's
 * limit is held at or below 900 mA and max77705-wireless reports an input
 * voltage limit of 5 V, which the receiver driver (p9320) takes as "no fast
 * wireless": a 10 V pad steps down to 5 V. Released when the boost ends.
 * With chg->lock held.
 */
static void max77705_otg_wc_set(struct max77705_charger_data *chg, bool lock)
{
	struct max77705_input_state *st = &chg->in[MAX77705_IN_WCIN];
	int cur;

	if (chg->otg_wc_lock == lock)
		return;
	chg->otg_wc_lock = lock;
	if (lock) {
		if (st->online == 1 && !max77705_ilim_read(chg, MAX77705_IN_WCIN, &cur) &&
		    cur > MAX77705_OTG_WCIN_UA) {
			cancel_delayed_work(&st->ramp_work);
			max77705_ilim_write(chg, MAX77705_IN_WCIN, MAX77705_OTG_WCIN_UA);
		}
	} else if (st->online == 1) {
		max77705_ramp_restart(chg, MAX77705_IN_WCIN);
	}
	power_supply_changed(chg->psy_wc);
}

/* Samsung max77705_set_otg(); call with chg->lock held */
static int max77705_otg_set(struct max77705_charger_data *chg, bool on)
{
	struct regmap *regmap = chg->regmap;
	unsigned int ok;
	int ret;

	if (on == chg->otg_on)
		return 0;

	if (on) {
		ret = regmap_read(regmap, MAX77705_CHG_REG_INT_OK, &ok);
		if (ret)
			return ret;
		/* never against a wired source; CHGIN is where the boost goes */
		if (ok & MAX77705_CHGIN_OK) {
			dev_warn(chg->dev, "OTG: a wired input is present (INT_OK %02x): no boost\n", ok);
			return -EBUSY;
		}
		/*
		 * Wireless (WCIN) is a separate input and keeps charging, as on
		 * stock (max77705_check_wcin_before_otg_on(), :766-797, lets
		 * OTG on with the pad online). Before the boost, stock locks
		 * the receiver at 5 V (set_otg :820-823 -> sec_battery
		 * CHARGE_OTG_CONTROL, WPC_VOUT_LOCK, sec_battery.c:4853-4893)
		 * and caps WCIN at wireless_otg_input_current, 900 mA
		 * (sec_battery.c:208-221, sec_battery.h:120).
		 */
		max77705_otg_wc_set(chg, true);
		if (max77705_wc_valid(ok))
			dev_info(chg->dev, "OTG: with wireless charging (WCIN %d mA max, receiver at 5 V)\n",
				 MAX77705_OTG_WCIN_UA / 1000);
		disable_irq_nosync(chg->chgin_irq);
		ret = regmap_field_write(chg->rfield[MAX77705_OTG_ILIM], MAX77705_OTG_ILIM_900);
		if (!ret)
			ret = regmap_field_write(chg->rfield[MAX77705_REG_FSW],
						 MAX77705_CHG_FSW_3MHz);
		if (!ret)
			ret = regmap_field_write(chg->rfield[MAX77705_REG_DISKIP],
						 MAX77705_DISABLE_SKIP);
		if (!ret) {
			chg->otg_on = true;	/* before CHGIN sees our 5 V */
			ret = regmap_update_bits(regmap, MAX77705_CHG_REG_CNFG_00,
						 MAX77705_OTG_CTRL, MAX77705_OTG_CTRL);
		}
		if (ret) {
			dev_err(chg->dev, "OTG: boost on failed: %d\n", ret);
			regmap_update_bits(regmap, MAX77705_CHG_REG_CNFG_00,
					   MAX77705_OTG_CTRL, 0);
			chg->otg_on = false;
			regmap_field_write(chg->rfield[MAX77705_REG_FSW],
					   MAX77705_CHG_FSW_1_5MHz);
			regmap_field_write(chg->rfield[MAX77705_REG_DISKIP],
					   MAX77705_AUTO_SKIP);
			enable_irq(chg->chgin_irq);
			max77705_otg_wc_set(chg, false);
			return ret;
		}
		enable_irq(chg->byp_irq);
		dev_info(chg->dev, "OTG: boost on (VBUS out, 900 mA limit)\n");
		return 0;
	}

	ret = regmap_update_bits(regmap, MAX77705_CHG_REG_CNFG_00, MAX77705_OTG_CTRL, 0);
	if (ret)
		dev_err(chg->dev, "OTG: boost off failed: %d\n", ret);
	msleep(50);
	disable_irq_nosync(chg->byp_irq);
	chg->otg_on = false;
	enable_irq(chg->chgin_irq);
	regmap_field_write(chg->rfield[MAX77705_REG_FSW], MAX77705_CHG_FSW_1_5MHz);
	regmap_field_write(chg->rfield[MAX77705_REG_DISKIP], MAX77705_AUTO_SKIP);
	dev_info(chg->dev, "OTG: boost off\n");
	max77705_otg_wc_set(chg, false);	/* stock: Vout lock released */
	/* look at the inputs again: CHGIN was ignored while boosting */
	queue_work(chg->wqueue, &chg->chgin_work);
	return ret;
}

static int max77705_otg_enable(struct regulator_dev *rdev)
{
	struct max77705_charger_data *chg = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&chg->lock);
	ret = max77705_otg_set(chg, true);
	mutex_unlock(&chg->lock);
	return ret;
}

static int max77705_otg_disable(struct regulator_dev *rdev)
{
	struct max77705_charger_data *chg = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&chg->lock);
	ret = max77705_otg_set(chg, false);
	mutex_unlock(&chg->lock);
	return ret;
}

static int max77705_otg_is_enabled(struct regulator_dev *rdev)
{
	struct max77705_charger_data *chg = rdev_get_drvdata(rdev);
	unsigned int v;
	int ret;

	ret = regmap_read(chg->regmap, MAX77705_CHG_REG_CNFG_00, &v);
	if (ret)
		return ret;
	return (v & MAX77705_OTG_CTRL) == MAX77705_OTG_CTRL;
}

static const struct regulator_ops max77705_otg_ops = {
	.enable		= max77705_otg_enable,
	.disable	= max77705_otg_disable,
	.is_enabled	= max77705_otg_is_enabled,
};

static const struct regulator_desc max77705_otg_desc = {
	.name		= "max77705-otg",
	.type		= REGULATOR_VOLTAGE,
	.owner		= THIS_MODULE,
	.ops		= &max77705_otg_ops,
	.n_voltages	= 1,
	.fixed_uV	= 5000000,
	.enable_time	= 20000,	/* stock gives the bus no settle time; 20 ms */
};

/* stock max77705_bypass_irq(): an over-current on the bypass path ends OTG */
static irqreturn_t max77705_byp_irq(int irq, void *data)
{
	struct max77705_charger_data *chg = data;
	unsigned int dtls = 0;
	bool oc = false;

	mutex_lock(&chg->lock);
	if (chg->otg_on && !regmap_read(chg->regmap, MAX77705_CHG_REG_DETAILS_02, &dtls) &&
	    (dtls & MAX77705_BYP_DTLS & 0x1)) {
		dev_warn(chg->dev, "OTG: bypass over-current (DETAILS_02 %02x): boost off\n", dtls);
		max77705_otg_set(chg, false);
		oc = true;
	}
	mutex_unlock(&chg->lock);
	if (oc && chg->otg_rdev)
		regulator_notifier_call_chain(chg->otg_rdev,
					      REGULATOR_EVENT_OVER_CURRENT, NULL);
	return IRQ_HANDLED;
}

static void max77705_otg_off(void *data)
{
	struct max77705_charger_data *chg = data;

	mutex_lock(&chg->lock);
	max77705_otg_set(chg, false);
	mutex_unlock(&chg->lock);
}

static void max77705_otg_put_node(void *np)
{
	of_node_put(np);
}

/* only with a "usb-otg-vbus" child in the DT: that is the board saying so */
static int max77705_otg_init(struct max77705_charger_data *chg,
			     struct regmap_irq_chip_data *irq_data)
{
	struct device *dev = chg->dev;
	struct regulator_config cfg = {};
	struct device_node *np;
	int ret;

	np = of_get_child_by_name(dev->of_node, "usb-otg-vbus");
	if (!np) {
		dev_info(dev, "no usb-otg-vbus node: no OTG VBUS\n");
		return 0;
	}
	ret = devm_add_action_or_reset(dev, max77705_otg_put_node, np);
	if (ret)
		return ret;

	chg->chgin_irq = regmap_irq_get_virq(irq_data, MAX77705_CHGIN_I);
	chg->byp_irq = regmap_irq_get_virq(irq_data, MAX77705_BYP_I);
	if (chg->chgin_irq <= 0 || chg->byp_irq <= 0)
		return dev_err_probe(dev, -EINVAL, "OTG: no CHGIN/BYP interrupt\n");
	ret = devm_request_threaded_irq(dev, chg->byp_irq, NULL, max77705_byp_irq,
					IRQF_TRIGGER_NONE | IRQF_NO_AUTOEN,
					"byp-irq", chg);
	if (ret)
		return dev_err_probe(dev, ret, "OTG: BYP interrupt\n");

	/* off on unbind: runs before the regulator is unregistered */
	cfg.dev = dev;
	cfg.driver_data = chg;
	cfg.of_node = np;
	cfg.init_data = of_get_regulator_init_data(dev, np, &max77705_otg_desc);
	chg->otg_rdev = devm_regulator_register(dev, &max77705_otg_desc, &cfg);
	if (IS_ERR(chg->otg_rdev)) {
		ret = PTR_ERR(chg->otg_rdev);
		chg->otg_rdev = NULL;
		return dev_err_probe(dev, ret, "OTG: regulator\n");
	}
	ret = devm_add_action_or_reset(dev, max77705_otg_off, chg);
	if (ret)
		return ret;
	dev_info(dev, "OTG VBUS regulator %s\n", rdev_get_name(chg->otg_rdev));
	return 0;
}

static void max77705_charger_shutdown(struct i2c_client *i2c)
{
	struct max77705_charger_data *chg = i2c_get_clientdata(i2c);

	if (chg && chg->otg_rdev)
		max77705_otg_off(chg);
}
/* ---- end zboot: OTG boost ------------------------------------------------- */
'''

patch(CFILE, [
    ('''#include <linux/regmap.h>
''', '''#include <linux/regmap.h>
#include <linux/of.h>			/* zboot: OTG boost */
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
'''),
    # the charger's own inputs are not inputs while we drive VBUS
    ('''	on[MAX77705_IN_WCIN] = max77705_wc_valid(ok);
''', '''	on[MAX77705_IN_WCIN] = max77705_wc_valid(ok);
	if (chg->otg_on) {	/* zboot: OTG boost: CHGIN's 5 V is ours */
		on[MAX77705_IN_CHGIN] = 0;
		/* WCIN_OK reads 0 under the boost without a pad (INT_OK 5f) */
		on[MAX77705_IN_WCIN] = !!(ok & MAX77705_WCIN_OK);
	}
'''),
    ('''	if (regmap_read(chg->regmap, MAX77705_CHG_REG_INT_OK, &ok))
		goto unmask;
	if (ok & MAX77705_CHGIN_OK)
		in = MAX77705_IN_CHGIN;
	else if''', '''	if (regmap_read(chg->regmap, MAX77705_CHG_REG_INT_OK, &ok))
		goto unmask;
	if (chg->otg_on) {	/* zboot: OTG boost: only WCIN is an input */
		if (!(ok & MAX77705_WCIN_OK))
			goto unmask;
		in = MAX77705_IN_WCIN;
	} else if (ok & MAX77705_CHGIN_OK)
		in = MAX77705_IN_CHGIN;
	else if'''),
    # WCIN under OTG: at most 900 mA (stock wireless_otg_input_current)
    ('''static int max77705_ramp_target(struct max77705_input_state *st)
{
	int hold = max77705_hold_ua(st);

	return hold ? min(hold, st->ceil_ua) : st->ceil_ua;
}''', '''/* zboot: OTG boost: stock wireless_otg_input_current (sec_battery.h:120) */
#define MAX77705_OTG_WCIN_UA	900000

static int max77705_ramp_target(struct max77705_input_state *st)
{
	int hold = max77705_hold_ua(st);
	int t = hold ? min(hold, st->ceil_ua) : st->ceil_ua;

	if (st->index == MAX77705_IN_WCIN && st->chg->otg_wc_lock)
		t = min(t, MAX77705_OTG_WCIN_UA);
	return t;
}'''),
    ('''static enum power_supply_property max77705_wc_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
};''', '''static enum power_supply_property max77705_wc_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT,	/* zboot: OTG boost: 5 V lock */
};'''),
    ('''	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		val->intval = READ_ONCE(chg->in[MAX77705_IN_WCIN].ceil_ua);
		return 0;
	default:
		return -EINVAL;
	}
}''', '''	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		val->intval = READ_ONCE(chg->in[MAX77705_IN_WCIN].ceil_ua);
		return 0;
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT:
		/* zboot: OTG boost: the receiver stays at 5 V (stock WPC_VOUT_LOCK) */
		if (!READ_ONCE(chg->otg_wc_lock))
			return -ENODATA;
		val->intval = 5000000;
		return 0;
	default:
		return -EINVAL;
	}
}'''),
    ('''	case POWER_SUPPLY_PROP_ONLINE:
		return max77705_get_online(regmap, &val->intval);''',
     '''	case POWER_SUPPLY_PROP_ONLINE:
		if (READ_ONCE(chg->otg_on)) {	/* zboot: OTG boost */
			val->intval = 0;
			return 0;
		}
		return max77705_get_online(regmap, &val->intval);'''),
    ('''		val->intval = max77705_wc_valid(ok);
		return 0;''', '''		/* zboot: OTG boost: CHGIN_OK is our boost then, WCIN_OK the pad */
		val->intval = READ_ONCE(chg->otg_on) ? !!(ok & MAX77705_WCIN_OK) :
			      max77705_wc_valid(ok);
		return 0;'''),
    ('''static void max77705_put_fwnode(void *fwnode)''',
     OTG_CODE.lstrip('\n') + '''
static void max77705_put_fwnode(void *fwnode)'''),
    ('''	ret = max77705_charger_enable(chg);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable charge\\n");''',
     '''	ret = max77705_otg_init(chg, irq_data);	/* zboot: OTG boost */
	if (ret)
		return ret;

	ret = max77705_charger_enable(chg);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable charge\\n");'''),
    ('''	.probe = max77705_charger_probe,
};''', '''	.probe = max77705_charger_probe,
	.shutdown = max77705_charger_shutdown,	/* zboot: OTG boost */
};'''),
])
print('patch-max77705-otg: OTG VBUS regulator (usb-otg-vbus)')
