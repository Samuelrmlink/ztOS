// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-nfc: the Galaxy S9's S3NRN82 NFC controller as /dev/zt-nfc, raw NCI.
 *
 * Stage 2 (card reading), owner-approved 2026-11-12. Everything the chip
 * needs is taken on open() and given back on release(), which the kernel
 * calls on every exit path of the user process (normal exit, error, kill):
 *
 *   open:    PMU CLKOUT1 (0x14060A0C, OSC_NFC) must read 0x00000001; write
 *            0x00000000 (enable, SEL = TCXO) and read back;
 *            zrn_i2c20_get(): bus i2c-103 (gpp5, class 0) and LDO38 on;
 *            VEN off/FIRM low 20 ms, VEN on 150 ms, WAKE high 20 ms;
 *            gpa1-2 IRQ (rising) requested; CLK_REQ (gpa2-6) sampled only.
 *   release: WAKE low, VEN high (chip and field off), IRQ freed,
 *            zrn_i2c20_put() (LDO38 restored), CLKOUT1 written 0x00000001
 *            and read back.
 *
 * write() takes one NCI packet and sends it to 0x27 only if nci-filter.h
 * allows it for the protocol currently activated. read() returns one NCI
 * packet from the chip (blocks up to 1 s, then -EAGAIN). A 30 s watchdog
 * starts with an accepted RF_DISCOVER: if no RF_DEACTIVATE is accepted by
 * then, the chip is switched off (VEN high), which ends the field.
 */
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "nci-filter.h"
#include "zrn-i2c20.h"

#define NFC_ADDR	0x27
#define EXP_CHIP	"96-0022"
#define EXP_VEN		11
#define EXP_FIRM	9
#define CLKOUT1_PHYS	0x14060A0C
#define CLKOUT1_GATED	0x00000001
#define CLKOUT1_ON	0x00000000
#define RF_MAX_MS	30000
#define PKT_MAX		(3 + 255)
#define RING		16

static struct gpiod_lookup_table ctl_gpios = {
	.dev_id = "zrn-nfc-ctl",
	.table = {
		GPIO_LOOKUP("gpa1", 2, "irq", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gpa2", 6, "clkreq", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP(EXP_CHIP, EXP_VEN, "ven", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP(EXP_CHIP, EXP_FIRM, "firm", GPIO_ACTIVE_HIGH),
		{ }
	},
};

static struct platform_device *ctl_pdev;
static struct gpio_desc *g_irq, *g_clkreq, *g_ven, *g_firm;

static DEFINE_MUTEX(dev_lock);		/* open/release/write */
static bool in_use;
static struct i2c_adapter *adap;
static int irq_num = -1;
static void __iomem *clkout;
static bool clk_enabled;

static DEFINE_SPINLOCK(ring_lock);
static struct { u16 len; u8 d[PKT_MAX]; } ring[RING];
static unsigned int r_head, r_tail;
static DECLARE_WAIT_QUEUE_HEAD(rxq);

static u8 active_proto;			/* from RF_INTF_ACTIVATED_NTF */
static bool chip_on, rf_running;
static struct delayed_work rf_watchdog;

/* ---------- chip I/O ---------- */

static int nfc_xfer(u16 flags, u8 *buf, u16 len)
{
	struct i2c_msg m = { .addr = NFC_ADDR, .flags = flags, .len = len, .buf = buf };
	int ret = i2c_transfer(adap, &m, 1);

	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

static void ring_push(const u8 *p, unsigned int len)
{
	unsigned long f;

	spin_lock_irqsave(&ring_lock, f);
	if (r_head - r_tail < RING) {
		ring[r_head % RING].len = len;
		memcpy(ring[r_head % RING].d, p, len);
		r_head++;
	} else {
		pr_warn("zrn-nfc: rx ring full, packet dropped\n");
	}
	spin_unlock_irqrestore(&ring_lock, f);
	wake_up_interruptible(&rxq);
}

static void track_rx(const u8 *p, unsigned int len)
{
	/* RF_INTF_ACTIVATED_NTF 61 05: id, interface, protocol, ... */
	if (len >= 6 && p[0] == 0x61 && p[1] == 0x05) {
		active_proto = p[5];
		pr_info("zrn-nfc: activated: interface %#x protocol %#x\n", p[4], p[5]);
	}
	/* RF_DEACTIVATE_NTF 61 06 */
	if (len >= 3 && p[0] == 0x61 && p[1] == 0x06) {
		active_proto = NCI_PROTO_NONE;
		if (len >= 4 && p[3] == 0x00) {		/* to idle: field off */
			rf_running = false;
			cancel_delayed_work(&rf_watchdog);
		}
	}
}

static irqreturn_t nfc_irq_thread(int irq, void *data)
{
	u8 pkt[PKT_MAX];
	int i;

	for (i = 0; i < 8 && chip_on; i++) {
		if (i && gpiod_get_value(g_irq) <= 0)
			break;
		if (nfc_xfer(I2C_M_RD, pkt, 3))
			break;
		if (pkt[2] && nfc_xfer(I2C_M_RD, pkt + 3, pkt[2]))
			break;
		track_rx(pkt, 3 + pkt[2]);
		ring_push(pkt, 3 + pkt[2]);
	}
	return IRQ_HANDLED;
}

static void chip_off(void)
{
	gpiod_set_value_cansleep(g_firm, 0);
	gpiod_set_value_cansleep(g_ven, 1);
	chip_on = false;
	rf_running = false;
	active_proto = NCI_PROTO_NONE;
}

static void rf_watchdog_fn(struct work_struct *w)
{
	mutex_lock(&dev_lock);
	if (rf_running && chip_on) {
		pr_warn("zrn-nfc: RF on for %d ms without RF_DEACTIVATE: chip off\n", RF_MAX_MS);
		chip_off();
	}
	mutex_unlock(&dev_lock);
}

/* ---------- clock ---------- */

static int clk_on(void)
{
	u32 v = readl(clkout);

	if (v != CLKOUT1_GATED) {
		pr_err("zrn-nfc: CLKOUT1 reads %#010x, expected %#010x: not touching it\n",
		       v, CLKOUT1_GATED);
		return -EIO;
	}
	writel(CLKOUT1_ON, clkout);
	v = readl(clkout);
	pr_info("zrn-nfc: CLKOUT1 %#010x -> %#010x\n", CLKOUT1_GATED, v);
	clk_enabled = true;
	if (v != CLKOUT1_ON) {
		pr_err("zrn-nfc: CLKOUT1 did not enable\n");
		return -EIO;
	}
	return 0;
}

static void clk_restore(void)
{
	u32 v;

	if (!clk_enabled)
		return;
	writel(CLKOUT1_GATED, clkout);
	v = readl(clkout);
	clk_enabled = false;
	pr_info("zrn-nfc: CLKOUT1 restored to %#010x (reads %#010x)\n", CLKOUT1_GATED, v);
}

/* ---------- file operations ---------- */

static void session_end(void)
{
	/* not _sync: the watchdog takes dev_lock, which the caller holds; a
	 * watchdog that runs late finds the chip already off */
	cancel_delayed_work(&rf_watchdog);
	chip_off();
	if (irq_num >= 0) {
		free_irq(irq_num, NULL);
		irq_num = -1;
	}
	if (adap) {
		zrn_i2c20_put(adap);	/* LDO38 restored when last user */
		adap = NULL;
	}
	clk_restore();
	pr_info("zrn-nfc: session closed: VEN=%d FIRM=%d CLK_REQ=%d\n",
		gpiod_get_value_cansleep(g_ven), gpiod_get_value_cansleep(g_firm),
		gpiod_get_value(g_clkreq));
}

static int nfc_open(struct inode *ino, struct file *f)
{
	int ret;

	mutex_lock(&dev_lock);
	if (in_use) {
		mutex_unlock(&dev_lock);
		return -EBUSY;
	}
	r_head = r_tail = 0;
	active_proto = NCI_PROTO_NONE;

	ret = clk_on();
	if (ret)
		goto fail;
	adap = zrn_i2c20_get();
	if (IS_ERR(adap)) {
		ret = PTR_ERR(adap);
		adap = NULL;
		goto fail;
	}

	/* power-up, NCI mode (never bootloader): the stage-1c sequence */
	gpiod_set_value_cansleep(g_ven, 1);
	gpiod_set_value_cansleep(g_firm, 0);
	msleep(20);
	gpiod_set_value_cansleep(g_ven, 0);
	msleep(150);
	gpiod_set_value_cansleep(g_firm, 1);
	msleep(20);
	chip_on = true;

	ret = request_threaded_irq(gpiod_to_irq(g_irq), NULL, nfc_irq_thread,
				   IRQF_TRIGGER_RISING | IRQF_ONESHOT, "zrn-nfc", NULL);
	if (ret)
		goto fail;
	irq_num = gpiod_to_irq(g_irq);
	pr_info("zrn-nfc: session open: CLK_REQ=%d IRQ=%d\n",
		gpiod_get_value(g_clkreq), gpiod_get_value(g_irq));
	in_use = true;
	mutex_unlock(&dev_lock);
	return 0;
fail:
	session_end();
	mutex_unlock(&dev_lock);
	return ret;
}

static int nfc_release(struct inode *ino, struct file *f)
{
	mutex_lock(&dev_lock);
	session_end();
	in_use = false;
	mutex_unlock(&dev_lock);
	return 0;
}

static ssize_t nfc_write(struct file *f, const char __user *ubuf, size_t len, loff_t *pos)
{
	u8 pkt[PKT_MAX];
	const char *why;
	int ret;

	if (len < 3 || len > PKT_MAX)
		return -EINVAL;
	if (copy_from_user(pkt, ubuf, len))
		return -EFAULT;

	mutex_lock(&dev_lock);
	if (!chip_on) {
		ret = -ENODEV;
		goto out;
	}
	why = zrn_nci_check(pkt, len, active_proto);
	if (why) {
		pr_warn("zrn-nfc: refused %*ph: %s\n", (int)min_t(size_t, len, 8), pkt, why);
		ret = -EPERM;
		goto out;
	}
	ret = nfc_xfer(0, pkt, len);
	if (ret == -ENXIO) {		/* chip in standby: one retry, as mainline */
		msleep(110);
		ret = nfc_xfer(0, pkt, len);
	}
	if (ret)
		goto out;
	if (zrn_nci_is_discover(pkt)) {
		rf_running = true;
		mod_delayed_work(system_wq, &rf_watchdog, msecs_to_jiffies(RF_MAX_MS));
	}
	ret = len;
out:
	mutex_unlock(&dev_lock);
	return ret;
}

static ssize_t nfc_read(struct file *f, char __user *ubuf, size_t len, loff_t *pos)
{
	u8 pkt[PKT_MAX];
	unsigned long fl;
	unsigned int n;
	long w;

	w = wait_event_interruptible_timeout(rxq, r_head != r_tail, HZ);
	if (w < 0)
		return w;
	if (w == 0)
		return -EAGAIN;
	spin_lock_irqsave(&ring_lock, fl);
	n = ring[r_tail % RING].len;
	memcpy(pkt, ring[r_tail % RING].d, n);
	r_tail++;
	spin_unlock_irqrestore(&ring_lock, fl);
	if (len < n)
		return -EMSGSIZE;
	if (copy_to_user(ubuf, pkt, n))
		return -EFAULT;
	return n;
}

static const struct file_operations nfc_fops = {
	.owner = THIS_MODULE,
	.open = nfc_open,
	.release = nfc_release,
	.read = nfc_read,
	.write = nfc_write,
	.llseek = noop_llseek,
};

static struct miscdevice nfc_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "zt-nfc",
	.fops = &nfc_fops,
	.mode = 0600,
};

/* ---------- module ---------- */

static void put_gpios(void)
{
	if (g_firm) {
		gpiod_set_value_cansleep(g_firm, 0);
		gpiod_put(g_firm);
	}
	if (g_ven) {
		gpiod_set_value_cansleep(g_ven, 1);	/* chip off; stays an output */
		gpiod_put(g_ven);
	}
	if (g_irq)
		gpiod_put(g_irq);
	if (g_clkreq)
		gpiod_put(g_clkreq);
	g_irq = g_clkreq = g_firm = g_ven = NULL;
}

static int __init zrn_nfc_init(void)
{
	struct device *dev;
	int ret;

	INIT_DELAYED_WORK(&rf_watchdog, rf_watchdog_fn);
	clkout = ioremap(CLKOUT1_PHYS, 4);
	if (!clkout)
		return -ENOMEM;
	gpiod_add_lookup_table(&ctl_gpios);
	ctl_pdev = platform_device_register_simple("zrn-nfc-ctl", -1, NULL, 0);
	if (IS_ERR(ctl_pdev)) {
		ret = PTR_ERR(ctl_pdev);
		goto err_table;
	}
	dev = &ctl_pdev->dev;

	g_ven = gpiod_get(dev, "ven", GPIOD_OUT_HIGH);		/* chip off */
	if (IS_ERR(g_ven)) { ret = PTR_ERR(g_ven); g_ven = NULL; goto err; }
	g_firm = gpiod_get(dev, "firm", GPIOD_OUT_LOW);
	if (IS_ERR(g_firm)) { ret = PTR_ERR(g_firm); g_firm = NULL; goto err; }
	g_clkreq = gpiod_get(dev, "clkreq", GPIOD_IN);
	if (IS_ERR(g_clkreq)) { ret = PTR_ERR(g_clkreq); g_clkreq = NULL; goto err; }
	g_irq = gpiod_get(dev, "irq", GPIOD_IN);
	if (IS_ERR(g_irq)) { ret = PTR_ERR(g_irq); g_irq = NULL; goto err; }

	ret = misc_register(&nfc_misc);
	if (ret)
		goto err;
	pr_info("zrn-nfc: /dev/zt-nfc ready (chip off)\n");
	return 0;
err:
	put_gpios();
	platform_device_unregister(ctl_pdev);
err_table:
	gpiod_remove_lookup_table(&ctl_gpios);
	iounmap(clkout);
	pr_err("zrn-nfc: setup failed %d\n", ret);
	return ret;
}

static void __exit zrn_nfc_exit(void)
{
	misc_deregister(&nfc_misc);	/* no open file can remain: module refcount */
	cancel_delayed_work_sync(&rf_watchdog);
	put_gpios();
	platform_device_unregister(ctl_pdev);
	gpiod_remove_lookup_table(&ctl_gpios);
	clk_restore();
	iounmap(clkout);
	pr_info("zrn-nfc: unloaded\n");
}

module_init(zrn_nfc_init);
module_exit(zrn_nfc_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Galaxy S9 S3NRN82 NFC: raw NCI char device with the stage-2 allowlist");
