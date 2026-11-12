// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-iris-probe: the iris experiment, steps 1 to 8 of camera/iris/README.md,
 * run once at load. Approved by the owner, attended, 2026-11-12. Never
 * autoloaded; run it only through camera/iris/run-iris.sh, which keeps the
 * mark that stops a retry after a hang.
 *
 * The sequence itself is iris-seq.h (host-tested by test-iris-seq.c). This
 * file only supplies the hardware operations:
 *   MMIO      CMU_PERIC1 gates, PERIC1 gpc1 bank, USI07_I2C (0x108D0000)
 *   S2MPB02   through zrn-flash's bit-banged adapter (i2c-97, addr 0x59),
 *             by adapter, so zrn-flash keeps its own client
 *   S2MPS18   through ACPM (s9p_acpm_pmic_update/read), enable field only
 *   reset     gpg2-3 through gpiolib
 * There is no operation for gpg0-7, for any SMC, or for a sensor register
 * write. The results are left in /sys/kernel/debug/zrn-iris-probe/result.
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>

#include "iris-seq.h"

int s9p_acpm_pmic_read(u8 reg, u8 *val);
int s9p_acpm_pmic_update(u8 reg, u8 val, u8 mask);

static int pb02_bus = 97;
module_param(pb02_bus, int, 0444);
MODULE_PARM_DESC(pb02_bus, "I2C adapter of the S2MPB02 (zrn-flash's i2c-gpio97)");

struct region {
	phys_addr_t base;
	size_t size;
	void __iomem *va;
};

static struct region regions[] = {
	{ CMU_PERIC1, 0x4000 },
	{ PERIC1_GPIO, 0x100 },
	{ HSI2C, 0x100 },
};

static struct i2c_adapter *pb02;
static struct platform_device *pdev;
static struct gpio_desc *reset_gpio;
static struct iris_result res;
static struct dentry *dir;
static char logbuf[4096];
static size_t loglen;

static struct gpiod_lookup_table reset_lookup = {
	.dev_id = "zrn-iris-probe",
	.table = {
		GPIO_LOOKUP("gpg2", 3, "reset", GPIO_ACTIVE_HIGH),
		{ }
	},
};

static void __iomem *va_of(unsigned int addr)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(regions); i++)
		if (addr >= regions[i].base && addr + 4 <= regions[i].base + regions[i].size)
			return regions[i].va + (addr - regions[i].base);
	return NULL;
}

static int k_mmio_read(void *c, unsigned int a, unsigned int *v)
{
	void __iomem *p = va_of(a);

	if (!p)
		return -EFAULT;
	*v = readl(p);
	return 0;
}

static int k_mmio_write(void *c, unsigned int a, unsigned int v)
{
	void __iomem *p = va_of(a);

	if (!p)
		return -EFAULT;
	writel(v, p);
	return 0;
}

static int k_pb02_read(void *c, unsigned char reg, unsigned char *val)
{
	union i2c_smbus_data d;
	int ret = i2c_smbus_xfer(pb02, 0x59, 0, I2C_SMBUS_READ, reg, I2C_SMBUS_BYTE_DATA, &d);

	if (!ret)
		*val = d.byte;
	return ret;
}

static int k_pb02_write(void *c, unsigned char reg, unsigned char val)
{
	union i2c_smbus_data d = { .byte = val };

	/* the one write this module may make to the IR LED chip */
	if (reg != PB02_FLED_CTRL2 || val != 0x00)
		return -EPERM;
	return i2c_smbus_xfer(pb02, 0x59, 0, I2C_SMBUS_WRITE, reg, I2C_SMBUS_BYTE_DATA, &d);
}

static int k_pmic_set(void *c, unsigned char reg, int on)
{
	u8 v = 0;
	int ret;

	if (reg != MPS_L39 && reg != MPS_L40 && reg != MPS_L41)
		return -EPERM;
	ret = s9p_acpm_pmic_update(reg, on ? MPS_ON : 0, MPS_ON);
	if (!ret)
		ret = s9p_acpm_pmic_read(reg, &v);
	if (ret)
		return ret;
	pr_info("zrn-iris: S2MPS18 0x%02x = 0x%02x\n", reg, v);
	return !!(v & MPS_ON) == !!on ? 0 : -EIO;
}

static int k_reset_set(void *c, int high)
{
	return gpiod_direction_output(reset_gpio, high);
}

static void k_delay_us(void *c, unsigned int us)
{
	if (us >= 1000)
		usleep_range(us, us + 100);
	else
		udelay(us);
}

static unsigned long k_now_us(void *c)
{
	return (unsigned long)ktime_to_us(ktime_get());
}

static void k_mark(void *c, const char *what)
{
	/* console=ramoops: this line survives a reset */
	pr_emerg("zrn-iris: %s\n", what);
	mdelay(20);
}

static __printf(2, 3) void k_log(void *c, const char *fmt, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vscnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	pr_info("zrn-iris: %s\n", line);
	if (loglen + n + 2 < sizeof(logbuf))
		loglen += scnprintf(logbuf + loglen, sizeof(logbuf) - loglen, "%s\n", line);
}

static int result_show(struct seq_file *m, void *v)
{
	seq_puts(m, logbuf);
	seq_printf(m, "result: %s step_failed=%d err=%d id=%04x version=%02x led_disarmed=%d\n",
		   res.id_ok ? "ANSWERED" : "NO-ANSWER", res.step_failed, res.err, res.id,
		   res.version, res.fled_disarmed);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(result);

static void unmap_all(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(regions); i++)
		if (regions[i].va) {
			iounmap(regions[i].va);
			regions[i].va = NULL;
		}
}

static int __init zrn_iris_init(void)
{
	static const struct iris_hw hw = {
		.mmio_read = k_mmio_read, .mmio_write = k_mmio_write,
		.pb02_read = k_pb02_read, .pb02_write = k_pb02_write,
		.pmic_set = k_pmic_set, .reset_set = k_reset_set,
		.delay_us = k_delay_us, .now_us = k_now_us, .mark = k_mark, .log = k_log,
	};
	unsigned int i;
	int ret;

	pb02 = i2c_get_adapter(pb02_bus);
	if (!pb02) {
		pr_err("zrn-iris: no i2c-%d (is zrn-flash loaded?)\n", pb02_bus);
		return -ENODEV;
	}
	if (strcmp(pb02->name, "i2c-gpio97")) {
		pr_err("zrn-iris: i2c-%d is \"%s\", not zrn-flash's bus\n", pb02_bus, pb02->name);
		ret = -ENODEV;
		goto put_adapter;
	}
	/* ioremap makes no access */
	for (i = 0; i < ARRAY_SIZE(regions); i++) {
		regions[i].va = ioremap(regions[i].base, regions[i].size);
		if (!regions[i].va) {
			ret = -ENOMEM;
			goto unmap;
		}
	}
	pdev = platform_device_register_simple("zrn-iris-probe", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		goto unmap;
	}
	gpiod_add_lookup_table(&reset_lookup);
	/* claimed as it is; driven only from step 2 on */
	reset_gpio = gpiod_get(&pdev->dev, "reset", GPIOD_ASIS);
	if (IS_ERR(reset_gpio)) {
		ret = PTR_ERR(reset_gpio);
		pr_err("zrn-iris: gpg2-3: %d\n", ret);
		goto lookup;
	}

	iris_run(&hw, &res);

	dir = debugfs_create_dir("zrn-iris-probe", NULL);
	debugfs_create_file("result", 0444, dir, NULL, &result_fops);
	pr_info("zrn-iris: done: %s, step_failed=%d\n", res.id_ok ? "sensor answered" : "no answer",
		res.step_failed);
	return 0;
lookup:
	gpiod_remove_lookup_table(&reset_lookup);
	platform_device_unregister(pdev);
unmap:
	unmap_all();
put_adapter:
	i2c_put_adapter(pb02);
	return ret;
}

static void __exit zrn_iris_exit(void)
{
	debugfs_remove_recursive(dir);
	gpiod_put(reset_gpio);	/* step 8 left it low */
	gpiod_remove_lookup_table(&reset_lookup);
	platform_device_unregister(pdev);
	unmap_all();
	i2c_put_adapter(pb02);
}

module_init(zrn_iris_init);
module_exit(zrn_iris_exit);
MODULE_DESCRIPTION("Galaxy S9 iris sensor reachability probe (ROADMAP 8.22)");
MODULE_LICENSE("GPL");
