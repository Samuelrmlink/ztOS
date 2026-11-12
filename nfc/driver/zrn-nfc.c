// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-nfc stage 1c: read the S3NRN82's NCI version, nothing else.
 *
 * Owner-approved access list (nfc/README.md section 7, 2026-11-12):
 *  - a bit-banged I2C bus on gpp5-0 (SCL) / gpp5-1 (SDA), bus ZRN_NFC_BUS,
 *    i2c-algo-bit with adapter class 0 (no detection probes), no retries;
 *    SDA/SCL levels are sampled (lines released = inputs) before power-up;
 *  - gpa1-2 as the chip's IRQ (rising edge); gpa2-6 (CLK_REQ) sampled only;
 *  - PCAL6524 (96-0022) line 11 = VEN (high = off) and line 9 = FIRM/WAKE,
 *    both outputs (the pca953x driver writes output reg 0x05, config 0x0D);
 *  - power up in NCI mode (never bootloader mode), CORE_RESET (keep config)
 *    and CORE_INIT to address 0x27 only, log the answers, power down.
 *  - stage 1c (2026-11-12): 150 ms after VEN on; on a NACK, CORE_RESET is
 *    sent once more after 110 ms (mainline s3fwrn5's standby retry).
 *
 * Never sent: RF_DISCOVER, CORE_SET_CONFIG, NFCEE commands, any proprietary
 * (GID 0xF) command, any bootloader command. No regulator and no clock is
 * touched. The whole sequence runs once at insmod; rmmod releases the bus.
 * VEN is left as an output driven high (chip off) when the module goes.
 */
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define ZRN_NFC_BUS	103
#define NFC_ADDR	0x27
#define EXP_CHIP	"96-0022"
#define EXP_VEN		11
#define EXP_FIRM	9

static struct platform_device *ctl_pdev;	/* consumer device for the GPIOs */

static struct gpiod_lookup_table ctl_gpios = {
	.dev_id = "zrn-nfc-ctl",
	.table = {
		GPIO_LOOKUP("gpa1", 2, "irq", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gpa2", 6, "clkreq", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP(EXP_CHIP, EXP_VEN, "ven", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP(EXP_CHIP, EXP_FIRM, "firm", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gpp5", 0, "scl", GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		GPIO_LOOKUP("gpp5", 1, "sda", GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		{ }
	},
};

static struct gpio_desc *g_irq, *g_clkreq, *g_ven, *g_firm, *g_scl, *g_sda;
static struct i2c_adapter *adap;
static bool bus_added;

static void set_sda(void *d, int v) { gpiod_set_value_cansleep(g_sda, v); }
static void set_scl(void *d, int v) { gpiod_set_value_cansleep(g_scl, v); }
static int get_sda(void *d) { return gpiod_get_value_cansleep(g_sda); }
static int get_scl(void *d) { return gpiod_get_value_cansleep(g_scl); }

static struct i2c_algo_bit_data bit = {
	.setsda = set_sda,
	.setscl = set_scl,
	.getsda = get_sda,
	.getscl = get_scl,
	.udelay = 5,		/* about 100 kHz */
	.timeout = HZ / 10,
};

static struct i2c_adapter nfc_adap = {
	.owner = THIS_MODULE,
	.name = "zrn-nfc gpp5",
	.class = 0,		/* no detection probing */
	.retries = 0,
	.algo_data = &bit,
	.nr = ZRN_NFC_BUS,
};
static DECLARE_COMPLETION(irq_done);
static int irq_num = -1;
static int irq_count;

static irqreturn_t nfc_irq(int irq, void *data)
{
	irq_count++;
	complete(&irq_done);
	return IRQ_HANDLED;
}

/* wait for the chip to signal data: the IRQ edge, or the line already high */
static bool wait_data(unsigned int ms)
{
	unsigned int i;

	for (i = 0; i < ms; i++) {
		if (try_wait_for_completion(&irq_done))
			return true;
		if (gpiod_get_value(g_irq) > 0)
			return true;
		usleep_range(1000, 1100);
	}
	return false;
}

static int nfc_xfer(u16 flags, u8 *buf, u16 len)
{
	struct i2c_msg m = { .addr = NFC_ADDR, .flags = flags, .len = len, .buf = buf };
	int ret = i2c_transfer(adap, &m, 1);

	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

static int nci_send(const u8 *cmd, u16 len)
{
	u8 buf[8];
	int ret;

	memcpy(buf, cmd, len);
	reinit_completion(&irq_done);
	ret = nfc_xfer(0, buf, len);
	pr_info("zrn-nfc: TX %*ph -> %d\n", len, cmd, ret);
	return ret;
}

/* read one NCI packet: 3-byte header, then the payload; returns length */
static int nci_recv(u8 *pkt, unsigned int max, unsigned int wait_ms)
{
	int ret, plen;

	if (!wait_data(wait_ms)) {
		pr_info("zrn-nfc: RX timeout after %u ms (irq line %d, irqs %d)\n",
			wait_ms, gpiod_get_value(g_irq), irq_count);
		return -ETIMEDOUT;
	}
	ret = nfc_xfer(I2C_M_RD, pkt, 3);
	if (ret) {
		pr_info("zrn-nfc: RX header read failed %d\n", ret);
		return ret;
	}
	plen = pkt[2];
	if (plen && 3 + plen <= max) {
		ret = nfc_xfer(I2C_M_RD, pkt + 3, plen);
		if (ret) {
			pr_info("zrn-nfc: RX payload read failed %d\n", ret);
			return ret;
		}
	}
	pr_info("zrn-nfc: RX %*ph\n", 3 + plen, pkt);
	reinit_completion(&irq_done);
	return 3 + plen;
}

static void sample(const char *when)
{
	pr_info("zrn-nfc: [%s] VEN=%d FIRM=%d IRQ=%d CLK_REQ=%d irqs=%d\n", when,
		gpiod_get_value_cansleep(g_ven), gpiod_get_value_cansleep(g_firm),
		gpiod_get_value(g_irq), gpiod_get_value(g_clkreq), irq_count);
}

static void run_version_read(void)
{
	static const u8 core_reset[] = { 0x20, 0x00, 0x01, 0x00 };	/* keep config */
	static const u8 core_init_v1[] = { 0x20, 0x01, 0x00 };
	static const u8 core_init_v2[] = { 0x20, 0x01, 0x02, 0x00, 0x00 };
	u8 pkt[260];
	int n, ver = 0, i;

	/* power-up, NCI mode: VEN off + FIRM low, VEN on, then WAKE high */
	gpiod_set_value_cansleep(g_ven, 1);
	gpiod_set_value_cansleep(g_firm, 0);
	msleep(20);
	sample("off");
	gpiod_set_value_cansleep(g_ven, 0);
	msleep(150);		/* stage 1c: vendor SEC_NFC_MARGINTIME */
	sample("ven-on");
	gpiod_set_value_cansleep(g_firm, 1);
	msleep(20);
	sample("wake");

	/* the chip may announce itself; read anything pending, never answer */
	if (gpiod_get_value(g_irq) > 0)
		nci_recv(pkt, sizeof(pkt), 1);

	n = nci_send(core_reset, sizeof(core_reset));
	if (n == -ENXIO || n == -EREMOTEIO) {
		/* stage 1c: one retry, as mainline does for a chip in standby */
		msleep(110);
		sample("retry");
		n = nci_send(core_reset, sizeof(core_reset));
	}
	if (n)
		goto off;
	n = nci_recv(pkt, sizeof(pkt), 500);
	if (n < 0)
		goto off;
	if (pkt[0] == 0x40 && pkt[1] == 0x00 && n >= 6) {
		ver = pkt[4];
		pr_info("zrn-nfc: CORE_RESET_RSP status %#x NCI version %d.%d config %#x\n",
			pkt[3], ver >> 4, ver & 0xf, pkt[5]);
	} else if (pkt[0] == 0x40 && pkt[1] == 0x00) {
		pr_info("zrn-nfc: CORE_RESET_RSP status %#x (NCI 2.x form), waiting for NTF\n",
			pkt[3]);
		n = nci_recv(pkt, sizeof(pkt), 500);
		if (n >= 8 && pkt[0] == 0x60 && pkt[1] == 0x00) {
			ver = pkt[5];
			pr_info("zrn-nfc: CORE_RESET_NTF trigger %#x cfg %#x NCI %d.%d mfr %#x info %*ph\n",
				pkt[3], pkt[4], ver >> 4, ver & 0xf, pkt[6], pkt[7],
				n > 8 ? pkt + 8 : pkt);
		}
	}

	if (ver >= 0x20)
		n = nci_send(core_init_v2, sizeof(core_init_v2));
	else
		n = nci_send(core_init_v1, sizeof(core_init_v1));
	if (n)
		goto off;
	n = nci_recv(pkt, sizeof(pkt), 500);
	if (n > 3 && pkt[0] == 0x40 && pkt[1] == 0x01 && ver < 0x20) {
		/* NCI 1.x CORE_INIT_RSP */
		u8 *p = pkt + 3;
		int nif = p[5];

		pr_info("zrn-nfc: CORE_INIT_RSP status %#x features %*ph RF interfaces %*ph\n",
			p[0], 4, p + 1, nif, p + 6);
		p += 6 + nif;
		if (p + 11 <= pkt + n)
			pr_info("zrn-nfc: max conns %d routing %d ctrl payload %d large params %d mfr %#x info %*ph\n",
				p[0], p[1] | p[2] << 8, p[3], p[4] | p[5] << 8, p[6], 4, p + 7);
	}

	/* drain notifications for 200 ms, log only */
	for (i = 0; i < 4; i++)
		if (nci_recv(pkt, sizeof(pkt), 50) < 0)
			break;
off:
	gpiod_set_value_cansleep(g_firm, 0);
	gpiod_set_value_cansleep(g_ven, 1);
	msleep(5);
	sample("powered-down");
}

static void put_gpios(void)
{
	if (irq_num >= 0)
		free_irq(irq_num, NULL);
	irq_num = -1;
	if (g_irq)
		gpiod_put(g_irq);
	if (g_clkreq)
		gpiod_put(g_clkreq);
	if (g_firm) {
		gpiod_set_value_cansleep(g_firm, 0);
		gpiod_put(g_firm);
	}
	if (g_ven) {
		gpiod_set_value_cansleep(g_ven, 1);	/* chip off; stays an output */
		gpiod_put(g_ven);
	}
	if (g_scl)
		gpiod_put(g_scl);
	if (g_sda)
		gpiod_put(g_sda);
	g_irq = g_clkreq = g_firm = g_ven = g_scl = g_sda = NULL;
}

static int __init zrn_nfc_init(void)
{
	struct device *dev;
	int ret;

	gpiod_add_lookup_table(&ctl_gpios);

	ctl_pdev = platform_device_register_simple("zrn-nfc-ctl", -1, NULL, 0);
	if (IS_ERR(ctl_pdev)) {
		ret = PTR_ERR(ctl_pdev);
		goto err_tables;
	}
	dev = &ctl_pdev->dev;

	/* VEN first, as an output already high (off), then FIRM low */
	g_ven = gpiod_get(dev, "ven", GPIOD_OUT_HIGH);
	if (IS_ERR(g_ven)) { ret = PTR_ERR(g_ven); g_ven = NULL; goto err; }
	g_firm = gpiod_get(dev, "firm", GPIOD_OUT_LOW);
	if (IS_ERR(g_firm)) { ret = PTR_ERR(g_firm); g_firm = NULL; goto err; }
	g_clkreq = gpiod_get(dev, "clkreq", GPIOD_IN);
	if (IS_ERR(g_clkreq)) { ret = PTR_ERR(g_clkreq); g_clkreq = NULL; goto err; }
	g_irq = gpiod_get(dev, "irq", GPIOD_IN);
	if (IS_ERR(g_irq)) { ret = PTR_ERR(g_irq); g_irq = NULL; goto err; }
	ret = gpiod_to_irq(g_irq);
	if (ret < 0)
		goto err;
	irq_num = ret;
	ret = request_irq(irq_num, nfc_irq, IRQF_TRIGGER_RISING, "zrn-nfc", NULL);
	if (ret) {
		irq_num = -1;
		goto err;
	}

	/* the bus lines, released (open drain high = input); sample their levels
	 * with the chip still off, before any transfer */
	g_scl = gpiod_get(dev, "scl", GPIOD_OUT_HIGH_OPEN_DRAIN);
	if (IS_ERR(g_scl)) { ret = PTR_ERR(g_scl); g_scl = NULL; goto err; }
	g_sda = gpiod_get(dev, "sda", GPIOD_OUT_HIGH_OPEN_DRAIN);
	if (IS_ERR(g_sda)) { ret = PTR_ERR(g_sda); g_sda = NULL; goto err; }
	usleep_range(100, 200);
	pr_info("zrn-nfc: bus idle SCL=%d SDA=%d\n", get_scl(NULL), get_sda(NULL));

	ret = i2c_bit_add_numbered_bus(&nfc_adap);
	if (ret)
		goto err;
	bus_added = true;
	adap = &nfc_adap;

	sample("start");
	run_version_read();
	return 0;

err:
	put_gpios();
	platform_device_unregister(ctl_pdev);
err_tables:
	gpiod_remove_lookup_table(&ctl_gpios);
	pr_err("zrn-nfc: setup failed %d\n", ret);
	return ret;
}

static void __exit zrn_nfc_exit(void)
{
	if (bus_added)
		i2c_del_adapter(&nfc_adap);
	put_gpios();
	platform_device_unregister(ctl_pdev);
	gpiod_remove_lookup_table(&ctl_gpios);
	pr_info("zrn-nfc: unloaded\n");
}

module_init(zrn_nfc_init);
module_exit(zrn_nfc_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Galaxy S9 S3NRN82 NFC: stage-1c NCI version read");
