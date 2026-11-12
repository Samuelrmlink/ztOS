// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-i2c20: the shared bus-owner for gpp5-0/1 (stock hsi2c20) and LDO38.
 * See zrn-i2c20.h. Host-tested (built) only so far.
 *
 * Accesses, the same as the owner-approved NFC stage 1b and HRM ID read:
 *  - load: a bit-banged i2c-gpio bus, number ZRN_I2C20_BUS, on gpp5-0/1
 *    (pins become GPIOs, open-drain emulation; pull-ups are external, on
 *    VDD_NFC_1P8). No transfer is made by this module itself.
 *  - first get: ACPM read of S2MPS18 0x6F; if bits 7:6 are 0, ACPM update
 *    0x6F value 0x80 mask 0xC0 (mode 2), read back, wait 5 ms.
 *  - last put (and unload): if this module switched it on, ACPM write of the
 *    value read before, then read back. leave_on=1 skips the restore.
 */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/machine.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_data/i2c-gpio.h>
#include <linux/platform_device.h>

#include "zrn-i2c20.h"

#define S2MPS18_L38CTRL		0x6f
#define LDO_MODE_MASK		0xc0
#define LDO_MODE_ON_PWREN	0x80	/* mode 2, the stock setting */

int s9p_acpm_pmic_read(u8 reg, u8 *val);
int s9p_acpm_pmic_update(u8 reg, u8 val, u8 mask);
int s9p_acpm_pmic_write(u8 reg, u8 val);

static bool leave_on;
module_param(leave_on, bool, 0644);
MODULE_PARM_DESC(leave_on, "keep LDO38 on after the last user (default: restore)");

static struct gpiod_lookup_table bus_gpios = {
	.dev_id = "i2c-gpio.103",
	.table = {
		GPIO_LOOKUP_IDX("gpp5", 0, NULL, 0, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		GPIO_LOOKUP_IDX("gpp5", 1, NULL, 1, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		{ }
	},
};

static struct i2c_gpio_platform_data bus_pdata = { .udelay = 5 };	/* ~100 kHz */
static struct platform_device *bus_pdev;

static DEFINE_MUTEX(lock);
static unsigned int users;
static bool we_switched_on;
static u8 ldo_before;

static int ldo_on(void)
{
	u8 v = 0;
	int ret = s9p_acpm_pmic_read(S2MPS18_L38CTRL, &v);

	if (ret)
		return ret;
	ldo_before = v;
	if (v & LDO_MODE_MASK) {
		pr_info("zrn-i2c20: LDO38 already on (%#04x)\n", v);
		we_switched_on = false;
		return 0;
	}
	ret = s9p_acpm_pmic_update(S2MPS18_L38CTRL, LDO_MODE_ON_PWREN, LDO_MODE_MASK);
	if (ret)
		return ret;
	ret = s9p_acpm_pmic_read(S2MPS18_L38CTRL, &v);
	if (ret)
		return ret;
	if ((v & LDO_MODE_MASK) != LDO_MODE_ON_PWREN) {
		pr_err("zrn-i2c20: LDO38 reads %#04x after the update\n", v);
		s9p_acpm_pmic_write(S2MPS18_L38CTRL, ldo_before);
		return -EIO;
	}
	we_switched_on = true;
	usleep_range(5000, 6000);	/* ramp: 12 mV/us, plus margin */
	pr_info("zrn-i2c20: LDO38 %#04x -> %#04x\n", ldo_before, v);
	return 0;
}

static void ldo_restore(void)
{
	u8 v = 0;

	if (!we_switched_on || leave_on)
		return;
	s9p_acpm_pmic_write(S2MPS18_L38CTRL, ldo_before);
	s9p_acpm_pmic_read(S2MPS18_L38CTRL, &v);
	pr_info("zrn-i2c20: LDO38 restored to %#04x (reads %#04x)\n", ldo_before, v);
	we_switched_on = false;
}

struct i2c_adapter *zrn_i2c20_get(void)
{
	struct i2c_adapter *adap;
	int ret = 0;

	mutex_lock(&lock);
	if (!users)
		ret = ldo_on();
	if (ret) {
		mutex_unlock(&lock);
		return ERR_PTR(ret);
	}
	adap = i2c_get_adapter(ZRN_I2C20_BUS);
	if (!adap) {
		if (!users)
			ldo_restore();
		mutex_unlock(&lock);
		return ERR_PTR(-ENODEV);
	}
	users++;
	mutex_unlock(&lock);
	return adap;
}
EXPORT_SYMBOL_GPL(zrn_i2c20_get);

void zrn_i2c20_put(struct i2c_adapter *adap)
{
	if (IS_ERR_OR_NULL(adap))
		return;
	i2c_put_adapter(adap);
	mutex_lock(&lock);
	if (!WARN_ON(!users) && !--users)
		ldo_restore();
	mutex_unlock(&lock);
}
EXPORT_SYMBOL_GPL(zrn_i2c20_put);

static int __init zrn_i2c20_init(void)
{
	struct i2c_adapter *adap;
	int ret;

	gpiod_add_lookup_table(&bus_gpios);
	bus_pdev = platform_device_register_data(NULL, "i2c-gpio", ZRN_I2C20_BUS,
						 &bus_pdata, sizeof(bus_pdata));
	if (IS_ERR(bus_pdev)) {
		ret = PTR_ERR(bus_pdev);
		goto err;
	}
	/* i2c-gpio probes synchronously; no adapter means gpp5 was taken */
	adap = i2c_get_adapter(ZRN_I2C20_BUS);
	if (!adap) {
		pr_err("zrn-i2c20: no bus %d (gpp5-0/1 in use?)\n", ZRN_I2C20_BUS);
		platform_device_unregister(bus_pdev);
		ret = -EBUSY;
		goto err;
	}
	i2c_put_adapter(adap);
	pr_info("zrn-i2c20: bus i2c-%d on gpp5-0/1\n", ZRN_I2C20_BUS);
	return 0;
err:
	gpiod_remove_lookup_table(&bus_gpios);
	return ret;
}

static void __exit zrn_i2c20_exit(void)
{
	/* clients hold module references through the symbols, so users is 0 */
	mutex_lock(&lock);
	ldo_restore();
	mutex_unlock(&lock);
	platform_device_unregister(bus_pdev);
	gpiod_remove_lookup_table(&bus_gpios);
	pr_info("zrn-i2c20: unloaded\n");
}

module_init(zrn_i2c20_init);
module_exit(zrn_i2c20_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Galaxy S9 hsi2c20 bus (gpp5) and LDO38 owner for NFC and HRM");
MODULE_SOFTDEP("pre: i2c-gpio");
