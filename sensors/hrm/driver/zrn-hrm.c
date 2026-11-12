// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-hrm: the Galaxy S9's heart rate / SpO2 sensor, a Maxim MAX86915 at 0x57
 * on the stock "hsi2c20" bus (gpp5-0/1), as a raw PPG source.
 *
 * NOT APPROVED TO RUN YET. Opening /dev/zrn_hrm switches the LED boost on and
 * drives the IR and red LEDs; that needs the owner's approval of the register
 * list in sensors/hrm/README.md ("Step 2 register list"). Loading the module
 * alone touches no hardware.
 *
 * Bus and pull-up rail (S2MPS18 LDO38) come from zrn-i2c20 (research/nfc).
 * The chip's own 1.8 V, S2MPB02 LDO8 (reg 0x25), is reached through bus 97,
 * which zrn-flash owns and zrn-cam shares.
 *
 * /dev/zrn_hrm, one reader at a time:
 *   open     power up, check PART_ID, configure, start (IR + red, flex mode)
 *   read     struct zrn_hrm_sample records (blocking unless O_NONBLOCK)
 *   poll     readable when samples are queued
 *   release  shut the chip down, boost off, LDO8 back, bus released
 *
 * No interrupt is used: a delayed work drains the FIFO (32 sample sets) every
 * 100 ms, so the interrupt pin (gpp3-0) stays as it is and the chip's
 * interrupt enables stay 0. At 100 sets/s that leaves 220 ms of margin.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/workqueue.h>

#include "zrn-hrm.h"
#include "zrn-i2c20.h"

#define NAME		"zrn-hrm"

/* S2MPB02 on bus 97 (leds/zrn-flash.c) */
#define PMIC_BUS	97
#define PMIC_ADDR	0x59
#define L8CTRL		0x25
#define L8_OFF_1V8	0x30
#define L8_ON_1V8	0xb0

/* MAX86915 (vendor drivers/optics/max86915.h) */
#define HRM_ADDR	0x57
#define R_INT_STATUS	0x00
#define R_INT_STATUS2	0x01
#define R_INT_ENABLE	0x02
#define R_INT_ENABLE2	0x03
#define R_FIFO_WR	0x04	/* then OVF 0x05, RD 0x06 */
#define R_FIFO_DATA	0x07
#define R_FIFO_CONFIG	0x08
#define R_MODE		0x09
#define R_MODE2		0x0a
#define R_LED1_PA	0x0c	/* IR */
#define R_LED2_PA	0x0d	/* red */
#define R_LED3_PA	0x0e	/* green */
#define R_LED4_PA	0x0f	/* blue */
#define R_LED_RANGE	0x11
#define R_LED_SEQ1	0x13
#define R_LED_SEQ2	0x14
#define R_XTALK1	0x26
#define R_XTALK2	0x27
#define R_REV_ID	0xfe
#define PART_ID		0x2b

#define MODE_RESET	0x40
#define MODE_SHDN	0x80
#define MODE_FLEX	0x03
#define FIFO_SETS	32
#define SET_BYTES	6	/* two slots (IR, red), 3 bytes each, 19 bits */

/*
 * The step 2 values (README). LED codes are capped at the approved maximum:
 * range 0 is 0.2 mA per step, so 0x30 is 9.6 mA.
 */
#define LED_MAX_CODE	0x30
static unsigned int led_ir = 0x30, led_red = 0x30;
module_param(led_ir, uint, 0444);
module_param(led_red, uint, 0444);
MODULE_PARM_DESC(led_ir, "IR LED code at range 0, 0.2 mA/step (max 0x30)");
MODULE_PARM_DESC(led_red, "red LED code at range 0, 0.2 mA/step (max 0x30)");

#define CFG_LED_RANGE	0x00	/* all LEDs range 0 */
#define CFG_SEQ1	0x21	/* slot 1 IR (1), slot 2 red (2) */
#define CFG_SEQ2	0x00
#define CFG_MODE2	0x6d	/* ADC range 3 (32 uA), SR 3 (400 Hz), PW 1 (120 us): stock */
#define CFG_FIFO	0x40	/* SMP_AVE 2 (4 samples) -> 100 sets/s, no rollover: stock */
#define POLL_MS		100

static struct gpiod_lookup_table boost_gpio = {
	.dev_id = NAME,
	.table = {
		GPIO_LOOKUP("gpg1", 6, "boost", GPIO_ACTIVE_HIGH),
		{ }
	},
};

static struct platform_device *pdev;
static struct i2c_adapter *pmic, *bus;
static struct gpio_desc *boost;
static int l8_was = -1;

static DEFINE_MUTEX(lock);		/* open/release, and the FIFO drain */
static bool running;
static DEFINE_KFIFO(samples, struct zrn_hrm_sample, 512);
static DECLARE_WAIT_QUEUE_HEAD(waitq);
static struct delayed_work drain;
static u32 seq;

/* ---- I2C helpers ---- */

static int smbus_rd(struct i2c_adapter *a, u16 addr, u8 reg)
{
	union i2c_smbus_data d;
	int ret = i2c_smbus_xfer(a, addr, 0, I2C_SMBUS_READ, reg,
				 I2C_SMBUS_BYTE_DATA, &d);

	return ret < 0 ? ret : d.byte;
}

static int smbus_wr(struct i2c_adapter *a, u16 addr, u8 reg, u8 val)
{
	union i2c_smbus_data d = { .byte = val };

	return i2c_smbus_xfer(a, addr, 0, I2C_SMBUS_WRITE, reg,
			      I2C_SMBUS_BYTE_DATA, &d);
}

/* register pointer, repeated start, len bytes (auto-increment) */
static int hrm_rd(u8 reg, u8 *buf, int len)
{
	struct i2c_msg m[] = {
		{ .addr = HRM_ADDR, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = HRM_ADDR, .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	int ret = i2c_transfer(bus, m, 2);

	return ret == 2 ? 0 : ret < 0 ? ret : -EIO;
}

static int hrm_wr(u8 reg, u8 val)
{
	return smbus_wr(bus, HRM_ADDR, reg, val);
}

/* ---- power ---- */

/* README step 2 list, 22-25: shutdown, boost off, LDO8 back, then the bus/LDO38 */
static void power_off(void)
{
	if (bus)
		hrm_wr(R_MODE, MODE_SHDN);
	if (boost) {
		gpiod_direction_input(boost);	/* the pin's pull-down holds it low */
		gpiod_put(boost);
		boost = NULL;
	}
	if (pmic) {
		if (l8_was >= 0) {
			smbus_wr(pmic, PMIC_ADDR, L8CTRL, l8_was);
			pr_info(NAME ": LDO8 back to %#04x (reads %#04x)\n", l8_was,
				smbus_rd(pmic, PMIC_ADDR, L8CTRL));
			l8_was = -1;
		}
		i2c_put_adapter(pmic);
		pmic = NULL;
	}
	if (bus) {
		zrn_i2c20_put(bus);
		bus = NULL;
	}
}

static int power_on(void)
{
	u8 id[2], st[2];
	int v, ret;
	const struct { u8 reg, val; } cfg[] = {
		{ R_LED1_PA, min_t(unsigned int, led_ir, LED_MAX_CODE) },
		{ R_LED2_PA, min_t(unsigned int, led_red, LED_MAX_CODE) },
		{ R_LED3_PA, 0 },
		{ R_LED4_PA, 0 },
		{ R_LED_RANGE, CFG_LED_RANGE },
		{ R_XTALK1, 0 },
		{ R_XTALK2, 0 },
		{ R_INT_ENABLE, 0 },
		{ R_INT_ENABLE2, 0 },
		{ R_LED_SEQ1, CFG_SEQ1 },
		{ R_LED_SEQ2, CFG_SEQ2 },
		{ R_MODE2, CFG_MODE2 },
		{ R_FIFO_CONFIG, CFG_FIFO },
		{ R_MODE, MODE_FLEX },	/* run */
	};
	int i;

	/* chip supply: S2MPB02 LDO8, only from the known "off, 1.8 V" state */
	pmic = i2c_get_adapter(PMIC_BUS);
	if (!pmic)
		return -ENODEV;
	v = smbus_rd(pmic, PMIC_ADDR, L8CTRL);
	if (v != L8_OFF_1V8) {
		pr_err(NAME ": LDO8 control reads %#x, expected %#x\n", v, L8_OFF_1V8);
		return v < 0 ? v : -EBUSY;
	}
	l8_was = v;
	ret = smbus_wr(pmic, PMIC_ADDR, L8CTRL, L8_ON_1V8);
	if (ret)
		return ret;

	/* bus and its pull-ups (LDO38) */
	bus = zrn_i2c20_get();
	if (IS_ERR(bus)) {
		ret = PTR_ERR(bus);
		bus = NULL;
		return ret;
	}
	usleep_range(2000, 3000);

	ret = hrm_rd(R_REV_ID, id, 2);
	if (ret)
		return ret;
	if (id[1] != PART_ID) {
		pr_err(NAME ": part %#04x, not a MAX86915\n", id[1]);
		return -ENODEV;
	}

	/* LED boost (gpg1-6), as the stock driver does before configuring */
	boost = gpiod_get(&pdev->dev, "boost", GPIOD_OUT_HIGH);
	if (IS_ERR(boost)) {
		ret = PTR_ERR(boost);
		boost = NULL;
		return ret;
	}
	usleep_range(1000, 1100);	/* stock: 1 ms after power-up */

	ret = hrm_wr(R_MODE, MODE_RESET);
	if (ret)
		return ret;
	usleep_range(1000, 1100);
	hrm_rd(R_INT_STATUS, st, 1);	/* read to clear */
	hrm_rd(R_INT_STATUS2, st + 1, 1);

	for (i = 0; i < ARRAY_SIZE(cfg); i++) {
		ret = hrm_wr(cfg[i].reg, cfg[i].val);
		if (ret) {
			pr_err(NAME ": write %#04x = %#04x: %d\n", cfg[i].reg, cfg[i].val, ret);
			return ret;
		}
	}
	pr_info(NAME ": MAX86915 rev %#04x running, IR %#04x red %#04x\n",
		id[0], cfg[0].val, cfg[1].val);
	return 0;
}

/* ---- FIFO ---- */

static u32 get19(const u8 *p)
{
	return ((p[0] << 16) | (p[1] << 8) | p[2]) & 0x7ffff;
}

static void drain_fifo(struct work_struct *w)
{
	u8 ptr[3], buf[FIFO_SETS * SET_BYTES];
	int n, i;
	u64 now;

	mutex_lock(&lock);
	if (!running)
		goto out;
	if (hrm_rd(R_FIFO_WR, ptr, 3))
		goto again;
	if (ptr[1] & 0x1f) {
		n = FIFO_SETS;	/* overflowed: take all, mark the gap */
		seq += ptr[1] & 0x1f;
	} else {
		n = ((ptr[0] & 0x1f) - (ptr[2] & 0x1f) + FIFO_SETS) % FIFO_SETS;
	}
	if (!n || hrm_rd(R_FIFO_DATA, buf, n * SET_BYTES))
		goto again;

	/* spread timestamps back from now at the nominal 10 ms per set */
	now = ktime_get_ns();
	for (i = 0; i < n; i++) {
		struct zrn_hrm_sample s = {
			.seq = seq++,
			.t_ns = now - (u64)(n - 1 - i) * 10000000ull,
			.ir = get19(buf + i * SET_BYTES),
			.red = get19(buf + i * SET_BYTES + 3),
		};

		kfifo_put(&samples, s);	/* drops the newest if the reader lags */
	}
	wake_up_interruptible(&waitq);
again:
	schedule_delayed_work(&drain, msecs_to_jiffies(POLL_MS));
out:
	mutex_unlock(&lock);
}

/* ---- /dev/zrn_hrm ---- */

static atomic_t opened = ATOMIC_INIT(0);

static int hrm_open(struct inode *ino, struct file *f)
{
	int ret;

	if (atomic_cmpxchg(&opened, 0, 1))
		return -EBUSY;
	mutex_lock(&lock);
	kfifo_reset(&samples);
	seq = 0;
	ret = power_on();
	if (ret) {
		power_off();
		mutex_unlock(&lock);
		atomic_set(&opened, 0);
		return ret;
	}
	running = true;
	schedule_delayed_work(&drain, msecs_to_jiffies(POLL_MS));
	mutex_unlock(&lock);
	return 0;
}

static int hrm_release(struct inode *ino, struct file *f)
{
	mutex_lock(&lock);
	running = false;
	mutex_unlock(&lock);
	cancel_delayed_work_sync(&drain);
	mutex_lock(&lock);
	power_off();
	mutex_unlock(&lock);
	atomic_set(&opened, 0);
	return 0;
}

static ssize_t hrm_read(struct file *f, char __user *ubuf, size_t len, loff_t *off)
{
	unsigned int copied;
	int ret;

	if (len < sizeof(struct zrn_hrm_sample))
		return -EINVAL;
	len -= len % sizeof(struct zrn_hrm_sample);
	while (kfifo_is_empty(&samples)) {
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(waitq, !kfifo_is_empty(&samples));
		if (ret)
			return ret;
	}
	ret = kfifo_to_user(&samples, ubuf, len, &copied);
	return ret ? ret : copied;
}

static __poll_t hrm_poll(struct file *f, poll_table *pt)
{
	poll_wait(f, &waitq, pt);
	return kfifo_is_empty(&samples) ? 0 : EPOLLIN | EPOLLRDNORM;
}

static const struct file_operations hrm_fops = {
	.owner = THIS_MODULE,
	.open = hrm_open,
	.release = hrm_release,
	.read = hrm_read,
	.poll = hrm_poll,
	.llseek = noop_llseek,
};

static struct miscdevice hrm_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "zrn_hrm",
	.fops = &hrm_fops,
	.mode = 0600,
};

static int __init zrn_hrm_init(void)
{
	int ret;

	INIT_DELAYED_WORK(&drain, drain_fifo);
	gpiod_add_lookup_table(&boost_gpio);
	pdev = platform_device_register_simple(NAME, -1, NULL, 0);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		goto err_table;
	}
	ret = misc_register(&hrm_misc);
	if (ret)
		goto err_pdev;
	pr_info(NAME ": /dev/zrn_hrm ready (nothing powered until opened)\n");
	return 0;
err_pdev:
	platform_device_unregister(pdev);
err_table:
	gpiod_remove_lookup_table(&boost_gpio);
	return ret;
}

static void __exit zrn_hrm_exit(void)
{
	misc_deregister(&hrm_misc);
	platform_device_unregister(pdev);
	gpiod_remove_lookup_table(&boost_gpio);
}

module_init(zrn_hrm_init);
module_exit(zrn_hrm_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Galaxy S9 MAX86915 raw PPG (IR + red) on zrn-i2c20");
MODULE_SOFTDEP("pre: zrn-i2c20");
