// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-hrm-probe: read the Galaxy S9 heart rate sensor's part ID, once.
 *
 * The owner approved exactly this (2026-11-12, "Approve ID read"), and
 * sensors/hrm/README.md lists it access by access:
 *
 *   1. S2MPB02 (bus 97, 0x59) register 0x25 (LDO8 = HRM_1.8V_AP): read it, and
 *      abort unless it is 0x30 (off, 1.8 V).
 *   2. Write 0x25 = 0xB0 (on, 1.8 V), read back, wait 2 ms.
 *   3. A bit-banged bus 99 on gpp5-0 (SCL) / gpp5-1 (SDA), the stock
 *      hsi2c@108E0000 pins. Lines are open-drain: "high" is the pin as an
 *      input, so no pull or other pinctrl setting is changed. The adapter has
 *      no class (no detection probes) and no address retries.
 *   4. If both lines read high: one 2-byte read of the MAX86915 at 0x57 from
 *      register 0xFE (REV_ID, PART_ID; 0x2B expected), at most 3 tries,
 *      10 ms apart.
 *   5. Remove bus 99, then write 0x25 back to the value read in step 1 and
 *      read it back.
 *
 * Nothing is written to the MAX86915, the LED boost enable (gpg1-6) and the
 * interrupt pin (gpp3-0) are not touched, and the NFC chip on the same bus
 * (0x27) is never addressed. The module always fails to load (-EAGAIN), so it
 * leaves nothing behind; the result is in dmesg.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define PMIC_BUS	97	/* leds/zrn-flash.c: the S2MPB02 on gpp6-0/1 */
#define PMIC_ADDR	0x59
#define L8CTRL		0x25
#define L8_OFF_1V8	0x30	/* bit 7 off, 600 mV + 0x30 * 25 mV */
#define L8_ON_1V8	0xb0

#define HRM_BUS		99
#define HRM_ADDR	0x57
#define HRM_REV_ID	0xfe	/* then PART_ID at 0xff */
#define HRM_TRIES	3

#define NAME		"zrn-hrm-probe"

static struct gpiod_lookup_table hrm_gpios = {
	.dev_id = NAME,
	.table = {
		GPIO_LOOKUP("gpp5", 1, "sda", GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		GPIO_LOOKUP("gpp5", 0, "scl", GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		{ }
	},
};

static struct gpio_desc *sda, *scl;

static void set_sda(void *d, int v) { gpiod_set_value_cansleep(sda, v); }
static void set_scl(void *d, int v) { gpiod_set_value_cansleep(scl, v); }
static int get_sda(void *d) { return gpiod_get_value_cansleep(sda); }
static int get_scl(void *d) { return gpiod_get_value_cansleep(scl); }

static struct i2c_algo_bit_data bit = {
	.setsda = set_sda,
	.setscl = set_scl,
	.getsda = get_sda,
	.getscl = get_scl,
	.udelay = 5,		/* about 100 kHz */
	.timeout = HZ / 10,
};

static struct i2c_adapter hrm_adap = {
	.owner = THIS_MODULE,
	.name = NAME,
	.class = 0,		/* no detection probing */
	.algo_data = &bit,
	.nr = HRM_BUS,
};

static int pmic_read(struct i2c_adapter *a, u8 reg)
{
	union i2c_smbus_data d;
	int ret = i2c_smbus_xfer(a, PMIC_ADDR, 0, I2C_SMBUS_READ, reg,
				 I2C_SMBUS_BYTE_DATA, &d);

	return ret < 0 ? ret : d.byte;
}

static int pmic_write(struct i2c_adapter *a, u8 reg, u8 val)
{
	union i2c_smbus_data d = { .byte = val };

	return i2c_smbus_xfer(a, PMIC_ADDR, 0, I2C_SMBUS_WRITE, reg,
			      I2C_SMBUS_BYTE_DATA, &d);
}

/* Steps 3 and 4: the bus, the line check and the one read. */
static void read_id(struct platform_device *pdev)
{
	u8 reg = HRM_REV_ID, id[2] = { 0, 0 };
	struct i2c_msg msgs[] = {
		{ .addr = HRM_ADDR, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = HRM_ADDR, .flags = I2C_M_RD, .len = 2, .buf = id },
	};
	int ret, i, l_sda, l_scl;

	/* open-drain high: the pins become inputs, nothing is driven */
	sda = gpiod_get(&pdev->dev, "sda", GPIOD_OUT_HIGH_OPEN_DRAIN);
	if (IS_ERR(sda)) {
		pr_err(NAME ": SDA gpp5-1: %ld\n", PTR_ERR(sda));
		return;
	}
	scl = gpiod_get(&pdev->dev, "scl", GPIOD_OUT_HIGH_OPEN_DRAIN);
	if (IS_ERR(scl)) {
		pr_err(NAME ": SCL gpp5-0: %ld\n", PTR_ERR(scl));
		goto put_sda;
	}

	l_sda = get_sda(NULL);
	l_scl = get_scl(NULL);
	pr_info(NAME ": lines with LDO8 on: SCL %d SDA %d\n", l_scl, l_sda);
	if (l_sda != 1 || l_scl != 1) {
		pr_err(NAME ": bus not idle-high, no transfer made\n");
		goto put_scl;
	}

	hrm_adap.dev.parent = &pdev->dev;
	ret = i2c_bit_add_numbered_bus(&hrm_adap);
	if (ret) {
		pr_err(NAME ": bus %d: %d\n", HRM_BUS, ret);
		goto put_scl;
	}
	hrm_adap.retries = 0;	/* algo-bit would resend a NAKed address 3 more times */

	for (i = 1; i <= HRM_TRIES; i++) {
		ret = i2c_transfer(&hrm_adap, msgs, ARRAY_SIZE(msgs));
		if (ret == ARRAY_SIZE(msgs))
			break;
		pr_info(NAME ": try %d: %d\n", i, ret);
		if (i < HRM_TRIES)
			msleep(10);
	}
	if (ret == ARRAY_SIZE(msgs))
		pr_info(NAME ": 0x%02x reg 0x%02x: rev 0x%02x part 0x%02x (%s), try %d\n",
			HRM_ADDR, HRM_REV_ID, id[0], id[1],
			id[1] == 0x2b ? "MAX86915" : "unexpected", i);
	else
		pr_err(NAME ": no answer from 0x%02x after %d tries\n", HRM_ADDR, HRM_TRIES);

	i2c_del_adapter(&hrm_adap);
put_scl:
	gpiod_put(scl);
put_sda:
	gpiod_put(sda);
}

static int __init zrn_hrm_probe_init(void)
{
	struct platform_device *pdev;
	struct i2c_adapter *pmic;
	int was, now, ret;

	pmic = i2c_get_adapter(PMIC_BUS);
	if (!pmic) {
		pr_err(NAME ": no bus %d (is zrn-flash loaded?)\n", PMIC_BUS);
		return -ENODEV;
	}

	/* step 1 */
	was = pmic_read(pmic, L8CTRL);
	pr_info(NAME ": S2MPB02 0x%02x = 0x%02x\n", L8CTRL, was);
	if (was != L8_OFF_1V8) {
		pr_err(NAME ": expected 0x%02x, stopping with nothing written\n", L8_OFF_1V8);
		ret = was < 0 ? was : -EINVAL;
		goto put_pmic;
	}

	/* step 2 */
	ret = pmic_write(pmic, L8CTRL, L8_ON_1V8);
	now = pmic_read(pmic, L8CTRL);
	pr_info(NAME ": S2MPB02 0x%02x <- 0x%02x: %d, reads 0x%02x\n",
		L8CTRL, L8_ON_1V8, ret, now);
	if (ret || now != L8_ON_1V8)
		goto restore;
	usleep_range(2000, 3000);

	/* steps 3 and 4 */
	gpiod_add_lookup_table(&hrm_gpios);
	pdev = platform_device_register_simple(NAME, -1, NULL, 0);
	if (IS_ERR(pdev))
		pr_err(NAME ": device: %ld\n", PTR_ERR(pdev));
	else {
		read_id(pdev);
		platform_device_unregister(pdev);
	}
	gpiod_remove_lookup_table(&hrm_gpios);

restore:
	/* step 5: always, whatever happened above */
	ret = pmic_write(pmic, L8CTRL, was);
	now = pmic_read(pmic, L8CTRL);
	pr_info(NAME ": S2MPB02 0x%02x <- 0x%02x (restore): %d, reads 0x%02x%s\n",
		L8CTRL, was, ret, now, now == was ? "" : "  ** NOT RESTORED **");
	ret = -EAGAIN;		/* nothing stays loaded */
put_pmic:
	i2c_put_adapter(pmic);
	return ret;
}
module_init(zrn_hrm_probe_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Galaxy S9 MAX86915 part-ID read, once (approved experiment)");
