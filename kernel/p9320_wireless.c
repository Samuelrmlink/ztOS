// SPDX-License-Identifier: GPL-2.0
/*
 * IDT P9320 wireless power receiver (Galaxy S9, "mfc-charger" in Samsung's
 * kernel), receiver side only.
 *
 * The P9320 runs its own firmware from on-chip MTP and is powered from the
 * coil, so it answers on I2C only while the phone lies on a pad (WPC_DET
 * high). Its VOUT feeds the MAX77705's WCIN input; whether wireless power is
 * actually flowing is the charger's business (max77705-wireless/online). This
 * driver reports what the receiver sees -- Vout, Iout, Vrect, operating
 * frequency, the pad's Samsung TX id -- as the power supply "p9320-wireless"
 * (type Wireless, no "online": present = a pad is there and the receiver
 * answers), and does what Samsung's driver does on attach so 5 V Qi charges
 * the way it does on stock:
 *
 *   - the mode-change interrupt enabled, INT_A acknowledged after each event
 *   - stock's foreign-object-detection calibration (FOD gain/offset pairs,
 *     tuned by Samsung for this coil and back cover) written at attach, the
 *     constant-voltage set from 85 % battery, the "stand at 5 V" set for a
 *     Samsung stand (TX id 0x30)
 *   - the RPP scaling coefficient
 *   - the TX id request (a Samsung proprietary packet, header 0x28 cmd 0x01)
 *     and "is the pad AFC capable?" (cmd 0x0C): both are questions, neither
 *     changes the pad's voltage
 *   - the RX over-voltage clamp when no TX id arrives (11 V; 13 V when HV)
 *
 * Samsung fast wireless (10 V, stage 2) is off unless userspace writes 1 to
 * hv_enable, and is dropped again by the driver itself when the battery is
 * warm (37.0 C, back at 35.0 C), full, a wired charger is online, or
 * hv_enable goes back to 0. Before the receiver's Vout goes up, the WCIN
 * input limit (max77705-wireless/input_current_limit) is capped at stock's
 * 500 mA ("prepare"), and while at 10 V it is held at or below 650 mA. The
 * driver only ever lowers that limit; raising it is the policy's job.
 * The system is kept awake while at 10 V so the thermal checks keep running.
 *
 * What is deliberately not here: firmware update or anything at 0x3000-0x3FFF
 * (M0 control, MTP), the MCU reset and LDO toggle commands, MST, A4WP, PMA
 * specifics beyond the FOD table, CS100 / end-of-power packets. Every register
 * write goes through an allowlist.
 *
 * Registers are 16-bit addresses, 8-bit data (two address bytes, then data).
 */
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm_wakeup.h>
#include <linux/power_supply.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define P9320_CHIP_ID_L		0x00
#define P9320_CHIP_ID_H		0x01
#define P9320_CHIP_REV		0x02
#define P9320_FW_MAJOR_L	0x04
#define P9320_FW_MINOR_L	0x06
#define P9320_STATUS_L		0x20
#define P9320_INT_A_L		0x22
#define P9320_INT_A_H		0x23
#define P9320_INT_A_EN_L	0x24
#define P9320_INT_A_CLR_L	0x26
#define P9320_INT_A_CLR_H	0x27
#define P9320_SYS_OP_MODE	0x2b	/* [7:5] pad mode */
#define P9320_ADC_VOUT		0x3c	/* mV, L/H */
#define P9320_VOUT_SET		0x3e	/* 3.5 V + 0.1 V * value */
#define P9320_ADC_VRECT		0x40	/* mV */
#define P9320_ADC_IOUT		0x44	/* mA */
#define P9320_ADC_DIE_TEMP	0x46	/* raw, 12 bits */
#define P9320_OP_FREQ		0x48	/* kHz */
#define P9320_CMD_L		0x4e
#define P9320_PKT_HEADER	0x50
#define P9320_PKT_CMD		0x51
#define P9320_PKT_DATA0		0x52
#define P9320_TX_DATA_CMD	0x58
#define P9320_TX_DATA_VAL	0x59
#define P9320_OV_CLAMP		0x6a
#define P9320_CM_FET		0x6b
#define P9320_FOD_WPC		0x70	/* 12 bytes */
#define P9320_FOD_PMA		0x80	/* 12 bytes */
#define P9320_RPP_SCALE		0xf0

#define P9320_FOD_LEN		12

/* INT_A low byte */
#define P9320_INT_OP_MODE	BIT(5)
/* INT_A high byte */
#define P9320_INT_TX_DATA	BIT(7)

/* AP2MFC command, low byte: the only two bits this driver ever sets */
#define P9320_CMD_SEND_RX_DATA	BIT(0)
#define P9320_CMD_CLEAR_INT	BIT(5)
#define P9320_CMD_ALLOWED	(P9320_CMD_SEND_RX_DATA | P9320_CMD_CLEAR_INT)

#define P9320_MODE_WPC_BASIC	1
#define P9320_MODE_WPC_ADV	2
#define P9320_MODE_PMA_SR1	3
#define P9320_MODE_PMA_SR1E	4

/* Samsung's packets: header 0x28 ("AFC_CONF"), command, one data byte */
#define P9320_HDR_AFC_CONF	0x28
#define P9320_RXCMD_TX_ID	0x01
#define P9320_RXCMD_AFC_SET	0x06
#define P9320_RXCMD_REQ_AFC_TX	0x0c
#define P9320_AFC_5V		0x05
#define P9320_AFC_10V		0x2c

/* what the pad says (TX_DATA_CMD) */
#define P9320_TXCMD_TX_ID	0x01
#define P9320_TXCMD_AFC_SET	0x02
#define P9320_TXCMD_CHG_ERR	0x05
#define P9320_TXCMD_WPS		0x07
#define P9320_TX_AFC_10V	0x01
#define P9320_TX_ID_STAND	0x30	/* stock's 5 V stand FOD: exactly 0x30 */

#define P9320_OV_CLAMP_13V	0x03
#define P9320_OV_CLAMP_11V	0x04

/* Vout set values (IDT), 5..10 V in 1 V steps */
/*
 * The first is the 5 V-class level: 5.5 V, stock's wpc_cc_cv_vout (0x157c in
 * the stock DT). At 5.0 V the MAX77705's WCIN input regulation (AICL) cut a
 * plain Qi pad to its 300 mA floor on the bench; stock keeps 0.5 V more.
 */
static const u8 p9320_vout_val[] = { 0x14, 0x19, 0x23, 0x2d, 0x37, 0x41 };
#define P9320_VOUT_5V		0
#define P9320_VOUT_10V		5

#define P9320_CMD_CNT		3	/* stock repeats packets this often */
#define P9320_TXID_TRIES	10
#define P9320_ATTACH_TRIES	10
#define P9320_MONITOR_MS	10000
#define P9320_HV_DELAY_MS	8000	/* stock: AFC 10 V request 8 s after attach */
#define P9320_STEP_MS		250
#define P9320_CV_CAPACITY	85	/* stock wireless_cc_cv */
#define P9320_HV_HOT_DC		370	/* battery temp, 0.1 C */
#define P9320_HV_COOL_DC	350
#define P9320_WCIN_PREPARE_UA	500000	/* stock cable-info PREPARE_WIRELESS_HV */
#define P9320_WCIN_HV_UA	650000	/* stock cable-info HV_WIRELESS */
#define P9320_HV_VRECT_MV	8500

enum p9320_hv {
	P9320_HV_OFF,		/* 5 V */
	P9320_HV_PREPARE,	/* pad agreed, waiting to switch */
	P9320_HV_ON,		/* 10 V */
	P9320_HV_STEPDOWN,	/* going back to 5 V */
};

static const char *const p9320_hv_names[] = {
	[P9320_HV_OFF] = "5v",
	[P9320_HV_PREPARE] = "prepare",
	[P9320_HV_ON] = "10v",
	[P9320_HV_STEPDOWN] = "stepdown",
};

enum p9320_fod {
	P9320_FOD_DEFAULT,	/* the firmware's own */
	P9320_FOD_CC,
	P9320_FOD_CV,
	P9320_FOD_STAND_5V,
};

static const char *const p9320_fod_names[] = {
	[P9320_FOD_DEFAULT] = "default",
	[P9320_FOD_CC] = "cc",
	[P9320_FOD_CV] = "cv",
	[P9320_FOD_STAND_5V] = "stand-5v",
};

struct p9320 {
	struct device *dev;
	struct i2c_client *client;
	struct power_supply *psy;
	struct gpio_desc *det_gpio;
	struct gpio_desc *int_gpio;
	int det_irq;
	int int_irq;
	bool afc_reasked;		/* the AFC question sent a second time */
	bool int_enabled;
	struct workqueue_struct *wq;	/* ordered: works never run in parallel */
	struct delayed_work attach_work;
	struct delayed_work txid_work;
	struct delayed_work txdata_work;
	struct delayed_work hv_work;
	struct delayed_work step_work;
	struct delayed_work monitor_work;
	struct mutex lock;		/* state and multi-register sequences */
	bool stopping;

	/* from the device tree */
	u8 fod_cc[P9320_FOD_LEN];
	u8 fod_cv[P9320_FOD_LEN];
	u8 fod_stand[P9320_FOD_LEN];
	bool have_fod_cc, have_fod_cv, have_fod_stand;
	u8 rpp;
	const char *battery_name;
	const char *wcin_name;
	const char *wired_name;

	/* per attach */
	bool attached;
	int attach_tries;
	u16 chip_id;
	u8 chip_rev;
	u16 fw_major, fw_minor;
	u8 mode;
	enum p9320_fod fod;
	int txid_tries;
	bool tx_id_done;
	u8 tx_id;
	u8 tx_err;
	bool hv_capable;
	bool hv_failed;
	enum p9320_hv hv;
	int vout_idx;
	bool hot;
	u16 last_int;

	/* userspace */
	bool hv_enable;

	/* the WCIN supply's changes (its OTG 5 V lock) reach the monitor */
	struct notifier_block psy_nb;
};

/* ---- I2C, with the write allowlist ---- */

static bool p9320_writable(u16 reg)
{
	switch (reg) {
	case P9320_INT_A_EN_L:
	case P9320_INT_A_CLR_L:
	case P9320_INT_A_CLR_H:
	case P9320_VOUT_SET:
	case P9320_CMD_L:
	case P9320_PKT_HEADER:
	case P9320_PKT_CMD:
	case P9320_PKT_DATA0:
	case P9320_OV_CLAMP:
	case P9320_CM_FET:
	case P9320_RPP_SCALE:
	case P9320_FOD_WPC ... P9320_FOD_WPC + P9320_FOD_LEN - 1:
	case P9320_FOD_PMA ... P9320_FOD_PMA + P9320_FOD_LEN - 1:
		return true;
	}
	return false;
}

static bool p9320_det(struct p9320 *p)
{
	return gpiod_get_value_cansleep(p->det_gpio) > 0;
}

static void p9320_io_error(struct p9320 *p, const char *op, u16 reg, int ret)
{
	/* off the pad the chip has no power: not answering is normal */
	if (p9320_det(p))
		dev_warn_ratelimited(p->dev, "%s 0x%02x failed: %d\n", op, reg, ret);
	else
		dev_dbg(p->dev, "%s 0x%02x failed off the pad: %d\n", op, reg, ret);
}

static int p9320_read(struct p9320 *p, u16 reg, u8 *val)
{
	u8 addr[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msg[2] = {
		{ .addr = p->client->addr, .len = 2, .buf = addr },
		{ .addr = p->client->addr, .flags = I2C_M_RD, .len = 1, .buf = val },
	};
	int ret;

	ret = i2c_transfer(p->client->adapter, msg, 2);
	if (ret != 2) {
		ret = ret < 0 ? ret : -EIO;
		p9320_io_error(p, "read", reg, ret);
		return ret;
	}
	return 0;
}

/* a little-endian pair, read a byte at a time the way stock does */
static int p9320_read16(struct p9320 *p, u16 reg, u16 *val)
{
	u8 l, h;
	int ret;

	ret = p9320_read(p, reg, &l);
	if (!ret)
		ret = p9320_read(p, reg + 1, &h);
	if (!ret)
		*val = l | (h << 8);
	return ret;
}

static int p9320_write(struct p9320 *p, u16 reg, u8 val)
{
	u8 buf[3] = { reg >> 8, reg & 0xff, val };
	int ret;

	if (WARN_ONCE(!p9320_writable(reg), "p9320: write to 0x%04x refused\n", reg))
		return -EPERM;
	if (reg == P9320_CMD_L && WARN_ONCE(val & ~P9320_CMD_ALLOWED,
			"p9320: command 0x%02x refused\n", val))
		return -EPERM;

	ret = i2c_master_send(p->client, buf, sizeof(buf));
	if (ret != sizeof(buf)) {
		ret = ret < 0 ? ret : -EIO;
		p9320_io_error(p, "write", reg, ret);
		return ret;
	}
	return 0;
}

static int p9320_update(struct p9320 *p, u16 reg, u8 mask, u8 val)
{
	u8 old;
	int ret;

	ret = p9320_read(p, reg, &old);
	if (ret)
		return ret;
	return p9320_write(p, reg, (old & ~mask) | (val & mask));
}

/*
 * Set a command bit; the firmware clears it when done (stock checks 3x).
 * Stock writes back whatever else is set; here only bits of our own are, so
 * a command still pending in another bit is never sent again.
 */
static int p9320_command(struct p9320 *p, u8 bit)
{
	int ret, i;
	u8 v;

	ret = p9320_read(p, P9320_CMD_L, &v);
	if (!ret)
		ret = p9320_write(p, P9320_CMD_L, (v & P9320_CMD_ALLOWED) | bit);
	if (ret)
		return ret;
	for (i = 0; i < 3; i++) {
		if (p9320_read(p, P9320_CMD_L, &v) || !(v & bit))
			break;
		usleep_range(1000, 2000);
	}
	return 0;
}

/* Header 0x28 packets to the pad, only the ones listed here. */
static int p9320_send_packet(struct p9320 *p, u8 cmd, u8 data)
{
	int ret;

	switch (cmd) {
	case P9320_RXCMD_TX_ID:
	case P9320_RXCMD_REQ_AFC_TX:
		if (data != 0x00)
			return -EPERM;
		break;
	case P9320_RXCMD_AFC_SET:
		if (data != P9320_AFC_5V && data != P9320_AFC_10V)
			return -EPERM;
		break;
	default:
		return -EPERM;
	}

	ret = p9320_write(p, P9320_PKT_HEADER, P9320_HDR_AFC_CONF);
	if (!ret)
		ret = p9320_write(p, P9320_PKT_CMD, cmd);
	if (!ret)
		ret = p9320_write(p, P9320_PKT_DATA0, data);
	if (!ret)
		ret = p9320_command(p, P9320_CMD_SEND_RX_DATA);
	return ret;
}

static int p9320_write_fod(struct p9320 *p, const u8 *fod, enum p9320_fod which)
{
	u16 base = (p->mode == P9320_MODE_PMA_SR1 || p->mode == P9320_MODE_PMA_SR1E) ?
		   P9320_FOD_PMA : P9320_FOD_WPC;
	int i, ret;

	for (i = 0; i < P9320_FOD_LEN; i++) {
		ret = p9320_write(p, base + i, fod[i]);
		if (ret)
			return ret;
	}
	p->fod = which;
	dev_info(p->dev, "FOD: %s table at 0x%02x\n", p9320_fod_names[which], base);
	return 0;
}

static int p9320_set_vout(struct p9320 *p, int idx)
{
	int ret;

	if (idx > P9320_VOUT_5V && !(p->hv_enable && p->hv_capable)) {
		dev_err(p->dev, "refusing Vout above 5 V without hv_enable\n");
		return -EPERM;
	}
	ret = p9320_write(p, P9320_VOUT_SET, p9320_vout_val[idx]);
	if (!ret)
		p->vout_idx = idx;
	return ret;
}

/* ---- the other power supplies (all optional; looked up by name) ---- */

static int p9320_psy_get(const char *name, enum power_supply_property psp, int *val)
{
	union power_supply_propval v;
	struct power_supply *psy;
	int ret;

	if (!name)
		return -ENODEV;
	psy = power_supply_get_by_name(name);
	if (!psy)
		return -ENODEV;
	ret = power_supply_get_property(psy, psp, &v);
	power_supply_put(psy);
	if (!ret)
		*val = v.intval;
	return ret;
}

/* Lower the WCIN input limit to at most @ua. Never raises it. */
static int p9320_cap_wcin(struct p9320 *p, int ua)
{
	union power_supply_propval v;
	struct power_supply *psy;
	int ret;

	psy = power_supply_get_by_name(p->wcin_name);
	if (!psy)
		return -ENODEV;
	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, &v);
	if (!ret && v.intval > ua) {
		dev_info(p->dev, "WCIN limit %d -> %d uA\n", v.intval, ua);
		v.intval = ua;
		ret = power_supply_set_property(psy, POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, &v);
	}
	power_supply_put(psy);
	return ret;
}

/* Battery temperature in 0.1 C updates p->hot with hysteresis. */
static void p9320_check_temp(struct p9320 *p)
{
	int t;

	if (p9320_psy_get(p->battery_name, POWER_SUPPLY_PROP_TEMP, &t)) {
		p->hot = true;		/* unknown counts as hot */
		return;
	}
	if (t >= P9320_HV_HOT_DC)
		p->hot = true;
	else if (t < P9320_HV_COOL_DC)
		p->hot = false;
}

/* Everything that must hold for 10 V. Updates p->hot. */
static bool p9320_hv_allowed(struct p9320 *p)
{
	struct power_supply *wcin;
	int v;

	if (!p->attached || !p->hv_enable || !p->hv_capable || p->stopping)
		return false;
	if (p->mode != P9320_MODE_WPC_BASIC && p->mode != P9320_MODE_WPC_ADV)
		return false;
	p9320_check_temp(p);
	if (p->hot)
		return false;
	if (!p9320_psy_get(p->battery_name, POWER_SUPPLY_PROP_STATUS, &v) &&
	    v == POWER_SUPPLY_STATUS_FULL)
		return false;
	if (!p9320_psy_get(p->wired_name, POWER_SUPPLY_PROP_ONLINE, &v) && v)
		return false;
	/*
	 * USB OTG: the charger holds WCIN at 5 V while its boost runs (it
	 * reports input_voltage_limit then), as stock locks the receiver's
	 * Vout at 5 V with OTG on (sec_battery.c:4853-4893, WPC_VOUT_LOCK).
	 */
	if (!p9320_psy_get(p->wcin_name, POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT, &v) &&
	    v < P9320_HV_VRECT_MV * 1000)
		return false;
	/* without the WCIN limit under our control there is no 10 V */
	wcin = power_supply_get_by_name(p->wcin_name);
	if (!wcin)
		return false;
	power_supply_put(wcin);
	return true;
}

static void p9320_queue(struct p9320 *p, struct delayed_work *w, unsigned int ms)
{
	if (!p->stopping)
		mod_delayed_work(p->wq, w, msecs_to_jiffies(ms));
}

static void p9320_set_hv(struct p9320 *p, enum p9320_hv hv)
{
	if (p->hv == hv)
		return;
	dev_info(p->dev, "fast wireless: %s -> %s\n",
		 p9320_hv_names[p->hv], p9320_hv_names[hv]);
	/* awake from the first step up to the last step down */
	if (p->hv == P9320_HV_OFF)
		pm_stay_awake(p->dev);
	else if (hv == P9320_HV_OFF)
		pm_relax(p->dev);
	p->hv = hv;
	power_supply_changed(p->psy);
}

static void p9320_start_stepdown(struct p9320 *p, const char *why)
{
	if (p->hv == P9320_HV_OFF || p->hv == P9320_HV_STEPDOWN)
		return;
	dev_info(p->dev, "back to 5 V: %s\n", why);
	cancel_delayed_work(&p->hv_work);
	if (p->hv == P9320_HV_PREPARE) {
		/* Vout never went up */
		p9320_set_hv(p, P9320_HV_OFF);
		return;
	}
	p9320_set_hv(p, P9320_HV_STEPDOWN);
	p9320_queue(p, &p->step_work, 0);
}

static void p9320_start_hv(struct p9320 *p)
{
	if (p->hv != P9320_HV_OFF || p->hv_failed || !p9320_hv_allowed(p))
		return;
	if (p9320_cap_wcin(p, P9320_WCIN_PREPARE_UA)) {
		dev_warn(p->dev, "cannot cap the WCIN limit, staying at 5 V\n");
		return;
	}
	p9320_set_hv(p, P9320_HV_PREPARE);
	p9320_queue(p, &p->hv_work, P9320_HV_DELAY_MS);
}

/* ---- attach / detach ---- */

static int p9320_identify(struct p9320 *p)
{
	u8 l, h;
	int ret;

	ret = p9320_read(p, P9320_CHIP_ID_L, &l);
	if (ret)
		return ret;
	ret = p9320_read(p, P9320_CHIP_ID_H, &h);
	if (ret)
		return ret;
	p->chip_id = l | (h << 8);
	ret = p9320_read(p, P9320_CHIP_REV, &p->chip_rev);
	if (!ret)
		ret = p9320_read16(p, P9320_FW_MAJOR_L, &p->fw_major);
	if (!ret)
		ret = p9320_read16(p, P9320_FW_MINOR_L, &p->fw_minor);
	return ret;
}

static void p9320_reset_state(struct p9320 *p)
{
	p->chip_id = 0;
	p->chip_rev = 0;
	p->fw_major = 0;
	p->fw_minor = 0;
	p->mode = 0;
	p->fod = P9320_FOD_DEFAULT;
	p->txid_tries = 0;
	p->afc_reasked = false;
	p->tx_id_done = false;
	p->tx_id = 0;
	p->tx_err = 0;
	p->hv_capable = false;
	p->hv_failed = false;
	p->vout_idx = P9320_VOUT_5V;
	p->last_int = 0;
}

static void p9320_attach(struct p9320 *p)
{
	u16 vout = 0, vrect = 0;
	u8 mode;

	dev_info(p->dev, "on a pad: chip 0x%04x rev 0x%02x fw %04x.%04x\n",
		 p->chip_id, p->chip_rev, p->fw_major, p->fw_minor);
	p->attached = true;

	/* stock: enable the mode-change interrupt */
	p9320_update(p, P9320_INT_A_EN_L, P9320_INT_OP_MODE, P9320_INT_OP_MODE);

	if (!p9320_read(p, P9320_SYS_OP_MODE, &mode))
		p->mode = mode >> 5;
	dev_info(p->dev, "pad mode %u\n", p->mode);

	if (p->have_fod_cc)
		p9320_write_fod(p, p->fod_cc, P9320_FOD_CC);

	/* the pad's answers raise INT: listen before asking it anything */
	if (!p->int_enabled) {
		enable_irq(p->int_irq);
		p->int_enabled = true;
	}

	/*
	 * A pad already at high voltage: the receiver kept stock's 10 V across
	 * a reboot. Treat it as HV; the monitor brings it down unless 10 V is
	 * allowed.
	 */
	if (!p9320_read16(p, P9320_ADC_VRECT, &vrect) &&
	    !p9320_read16(p, P9320_ADC_VOUT, &vout) &&
	    vrect >= P9320_HV_VRECT_MV && vout >= P9320_HV_VRECT_MV) {
		dev_info(p->dev, "already at HV: Vrect %u mV Vout %u mV\n", vrect, vout);
		p->hv_capable = true;
		p->vout_idx = P9320_VOUT_10V;
		p->hv = P9320_HV_ON;
		pm_stay_awake(p->dev);
		if (p9320_hv_allowed(p))
			p9320_cap_wcin(p, P9320_WCIN_HV_UA);
		else
			p9320_start_stepdown(p, "fast wireless not allowed");
	} else {
		/* stock's charging Vout, 5.5 V (the chip starts at 5.0 V) */
		if (!p9320_write(p, P9320_VOUT_SET, p9320_vout_val[P9320_VOUT_5V]))
			dev_info(p->dev, "Vout 5.5 V (stock's wpc_cc_cv_vout)\n");
		/* a question: the pad answers AFC_SET with what it can do */
		p9320_send_packet(p, P9320_RXCMD_REQ_AFC_TX, 0x00);
	}

	p9320_write(p, P9320_RPP_SCALE, p->rpp);

	p9320_queue(p, &p->txid_work, 2500);
	p9320_queue(p, &p->monitor_work, P9320_MONITOR_MS);
	power_supply_changed(p->psy);
}

static void p9320_detach(struct p9320 *p)
{
	dev_info(p->dev, "off the pad\n");
	p->attached = false;
	if (p->int_enabled) {
		disable_irq_nosync(p->int_irq);
		p->int_enabled = false;
	}
	cancel_delayed_work(&p->txid_work);
	cancel_delayed_work(&p->txdata_work);
	cancel_delayed_work(&p->hv_work);
	cancel_delayed_work(&p->step_work);
	cancel_delayed_work(&p->monitor_work);
	if (p->hv != P9320_HV_OFF)
		pm_relax(p->dev);
	p->hv = P9320_HV_OFF;
	p9320_reset_state(p);
	power_supply_changed(p->psy);
}

static void p9320_attach_work(struct work_struct *work)
{
	struct p9320 *p = container_of(work, struct p9320, attach_work.work);
	bool det;

	mutex_lock(&p->lock);
	det = p9320_det(p);
	if (det && !p->attached) {
		if (p9320_identify(p)) {
			if (++p->attach_tries < P9320_ATTACH_TRIES)
				p9320_queue(p, &p->attach_work, 500);
			else if (p->attach_tries == P9320_ATTACH_TRIES)
				dev_info(p->dev, "WPC_DET is high but the receiver does not answer\n");
		} else {
			p->attach_tries = 0;
			p9320_attach(p);
		}
	} else if (!det) {
		p->attach_tries = 0;
		if (p->attached)
			p9320_detach(p);
	}
	mutex_unlock(&p->lock);
}

/* ---- what the pad says ---- */

static const char *p9320_tx_class(u8 id)
{
	switch (id) {
	case 0x00:
		return "unknown";
	case 0x11:
		return "vehicle";
	case 0x01 ... 0x10:
	case 0x12 ... 0x1f:
		return "pad";
	case 0x20 ... 0x2f:
		return "multi-port";
	case 0x30 ... 0x3f:
		return "stand";
	case 0x40 ... 0x4f:
		return "battery-pack";
	case 0x72:
	case 0x80 ... 0x9f:
		return "phone";
	}
	return "other";
}

static void p9320_txdata_work(struct work_struct *work)
{
	struct p9320 *p = container_of(work, struct p9320, txdata_work.work);
	u8 cmd, val;

	mutex_lock(&p->lock);
	if (!p->attached || p9320_read(p, P9320_TX_DATA_CMD, &cmd) ||
	    p9320_read(p, P9320_TX_DATA_VAL, &val))
		goto out;

	dev_info(p->dev, "from the pad: cmd 0x%02x data 0x%02x\n", cmd, val);
	switch (cmd) {
	case P9320_TXCMD_AFC_SET:
		if (val == P9320_TX_AFC_10V && !p->hv_capable) {
			p->hv_capable = true;
			power_supply_changed(p->psy);
		}
		if (val == P9320_TX_AFC_10V)
			p9320_start_hv(p);
		break;
	case P9320_TXCMD_TX_ID:
		if (p->tx_id_done)
			break;
		p->tx_id_done = true;
		p->tx_id = val;
		cancel_delayed_work(&p->txid_work);
		dev_info(p->dev, "TX id 0x%02x (%s)\n", val, p9320_tx_class(val));
		if (val == P9320_TX_ID_STAND && p->hv == P9320_HV_OFF &&
		    p->have_fod_stand && p->fod != P9320_FOD_CV)
			p9320_write_fod(p, p->fod_stand, P9320_FOD_STAND_5V);
		power_supply_changed(p->psy);
		break;
	case P9320_TXCMD_CHG_ERR:
		/* 0x12 OTP, 0x13 OCP, 0x14 misaligned, 0x20-0x27 FOD */
		p->tx_err = val;
		dev_warn(p->dev, "pad reports error 0x%02x\n", val);
		power_supply_changed(p->psy);
		break;
	default:
		break;
	}
out:
	mutex_unlock(&p->lock);
}

/* INT_A: read, note, acknowledge (0x26/0x27, then CMD bit 5) as stock does */
static void p9320_handle_int(struct p9320 *p)
{
	u8 l, h;

	if (!p->attached || p9320_read(p, P9320_INT_A_L, &l) ||
	    p9320_read(p, P9320_INT_A_H, &h))
		return;
	p->last_int = l | (h << 8);
	dev_dbg(p->dev, "INT_A 0x%04x\n", p->last_int);

	if (l & P9320_INT_OP_MODE) {
		u8 mode;

		if (!p9320_read(p, P9320_SYS_OP_MODE, &mode))
			dev_info(p->dev, "mode change: 0x%02x\n", mode);
	}
	if (l & (BIT(4) | BIT(3) | BIT(2)))
		dev_warn(p->dev, "receiver over-%s%s%s\n",
			 (l & BIT(4)) ? "voltage " : "",
			 (l & BIT(3)) ? "current " : "",
			 (l & BIT(2)) ? "temperature" : "");
	/* stock gives the packet 500 ms */
	if (h & P9320_INT_TX_DATA)
		p9320_queue(p, &p->txdata_work, 500);

	if (l || h) {
		p9320_write(p, P9320_INT_A_CLR_L, l);
		p9320_write(p, P9320_INT_A_CLR_H, h);
		p9320_command(p, P9320_CMD_CLEAR_INT);
	}
}

static irqreturn_t p9320_int_irq(int irq, void *data)
{
	struct p9320 *p = data;

	if (!p9320_det(p))
		return IRQ_HANDLED;	/* unpowered, nothing to acknowledge */
	mutex_lock(&p->lock);
	p9320_handle_int(p);
	mutex_unlock(&p->lock);
	return IRQ_HANDLED;
}

static irqreturn_t p9320_det_irq(int irq, void *data)
{
	struct p9320 *p = data;

	p9320_queue(p, &p->attach_work, 100);
	return IRQ_HANDLED;
}

/* ---- periodic: TX id, monitor, 10 V switch and step-down ---- */

static void p9320_txid_work(struct work_struct *work)
{
	struct p9320 *p = container_of(work, struct p9320, txid_work.work);

	mutex_lock(&p->lock);
	if (!p->attached || p->tx_id_done)
		goto out;
	/*
	 * The AFC question at attach can be answered before the pad is ready
	 * to talk (a Samsung fast pad answered only its TX id, 0xf0, on the
	 * bench): ask once more now, before the TX id requests.
	 */
	if (!p->afc_reasked && !p->hv_capable && p->hv == P9320_HV_OFF) {
		p->afc_reasked = true;
		dev_info(p->dev, "asking the pad for AFC again\n");
		p9320_send_packet(p, P9320_RXCMD_REQ_AFC_TX, 0x00);
		p9320_queue(p, &p->txid_work, 700);
		goto out;
	}
	if (p->txid_tries++ < P9320_TXID_TRIES) {
		p9320_send_packet(p, P9320_RXCMD_TX_ID, 0x00);
		p9320_queue(p, &p->txid_work, 1500);
	} else {
		/* stock: no TX id, clamp the receiver's over-voltage */
		dev_info(p->dev, "no TX id from the pad\n");
		p9320_write(p, P9320_OV_CLAMP, p->hv == P9320_HV_OFF ?
			    P9320_OV_CLAMP_11V : P9320_OV_CLAMP_13V);
	}
out:
	mutex_unlock(&p->lock);
}

static void p9320_monitor_work(struct work_struct *work)
{
	struct p9320 *p = container_of(work, struct p9320, monitor_work.work);
	int cap;

	mutex_lock(&p->lock);
	if (!p->attached)
		goto out;

	/* stock: the constant-voltage FOD set from 85 % on, until removal */
	if (p->have_fod_cv && p->fod != P9320_FOD_CV &&
	    !p9320_psy_get(p->battery_name, POWER_SUPPLY_PROP_CAPACITY, &cap) &&
	    cap >= P9320_CV_CAPACITY)
		p9320_write_fod(p, p->fod_cv, P9320_FOD_CV);

	switch (p->hv) {
	case P9320_HV_OFF:
		p9320_start_hv(p);
		break;
	case P9320_HV_PREPARE:
	case P9320_HV_ON:
		if (!p9320_hv_allowed(p))
			p9320_start_stepdown(p, p->hot ? "battery warm" :
					     "fast wireless not allowed");
		else
			p9320_cap_wcin(p, p->hv == P9320_HV_ON ?
				       P9320_WCIN_HV_UA : P9320_WCIN_PREPARE_UA);
		break;
	default:
		break;
	}

	/* an edge lost while the line stayed low */
	if (gpiod_get_value_cansleep(p->int_gpio) > 0)
		p9320_handle_int(p);

	p9320_queue(p, &p->monitor_work, P9320_MONITOR_MS);
out:
	mutex_unlock(&p->lock);
}

static void p9320_hv_work(struct work_struct *work)
{
	struct p9320 *p = container_of(work, struct p9320, hv_work.work);
	u16 vrect = 0, vout = 0;
	u8 cm_fet;
	int i;

	mutex_lock(&p->lock);
	if (p->hv != P9320_HV_PREPARE)
		goto out;
	if (!p9320_hv_allowed(p)) {
		p9320_set_hv(p, P9320_HV_OFF);
		goto out;
	}
	/* the WCIN limit comes down before anything goes up */
	if (p9320_cap_wcin(p, P9320_WCIN_PREPARE_UA)) {
		p9320_set_hv(p, P9320_HV_OFF);
		goto out;
	}

	for (i = 0; i < P9320_CMD_CNT; i++) {
		p9320_send_packet(p, P9320_RXCMD_AFC_SET, P9320_AFC_10V);
		msleep(100);
	}
	/*
	 * Stock (mfc_wpc_afc_vout_work, then the vout mode work) sets the
	 * receiver's Vout to 10 V straight after telling the pad, without waiting:
	 * the pad raises Vrect in answer to the receiver's own control-error
	 * packets for the higher Vout. Waiting for Vrect first, as this driver
	 * did, never happens (a Samsung fast pad held 5.56 V on the bench).
	 * So: all communication-modulation FETs, Vout 10 V, then check both rose.
	 */
	if (p9320_read(p, P9320_CM_FET, &cm_fet))
		cm_fet = 0xff;
	p9320_write(p, P9320_CM_FET, 0x00);
	if (p9320_set_vout(p, P9320_VOUT_10V))
		goto back_to_5v;
	for (i = 0; i < 30; i++) {
		if (!p9320_det(p))
			goto out;
		if (!p9320_read16(p, P9320_ADC_VRECT, &vrect) && !p9320_read16(p, P9320_ADC_VOUT, &vout) &&
		    vrect >= P9320_HV_VRECT_MV && vout >= P9320_HV_VRECT_MV)
			break;
		msleep(100);
	}
	if (vrect < P9320_HV_VRECT_MV || vout < P9320_HV_VRECT_MV) {
		dev_warn(p->dev, "pad did not follow to 10 V (Vrect %u mV, Vout %u mV): back to 5 V\n", vrect, vout);
		goto back_to_5v;
	}
	dev_info(p->dev, "fast wireless: Vrect %u mV, Vout %u mV\n", vrect, vout);
	p9320_set_hv(p, P9320_HV_ON);
	goto out;

back_to_5v:
	p->hv_failed = true;
	p9320_write(p, P9320_VOUT_SET, p9320_vout_val[P9320_VOUT_5V]);
	p->vout_idx = P9320_VOUT_5V;
	if (cm_fet != 0xff)
		p9320_write(p, P9320_CM_FET, cm_fet);
	p9320_send_packet(p, P9320_RXCMD_AFC_SET, P9320_AFC_5V);
	p9320_set_hv(p, P9320_HV_OFF);
out:
	mutex_unlock(&p->lock);
}

/* stock WIRELESS_VOUT_5V_STEP: one volt every 250 ms, then tell the pad */
static void p9320_step_work(struct work_struct *work)
{
	struct p9320 *p = container_of(work, struct p9320, step_work.work);
	int i;

	mutex_lock(&p->lock);
	if (!p->attached || p->hv != P9320_HV_STEPDOWN)
		goto out;
	if (p->vout_idx > P9320_VOUT_5V) {
		if (!p9320_write(p, P9320_VOUT_SET, p9320_vout_val[p->vout_idx - 1]))
			p->vout_idx--;
		p9320_queue(p, &p->step_work, P9320_STEP_MS);
		goto out;
	}
	for (i = 0; i < P9320_CMD_CNT; i++) {
		p9320_send_packet(p, P9320_RXCMD_AFC_SET, P9320_AFC_5V);
		msleep(100);
	}
	p9320_set_hv(p, P9320_HV_OFF);
out:
	mutex_unlock(&p->lock);
}

/* ---- power supply ---- */

static enum power_supply_property p9320_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static int p9320_get_property(struct power_supply *psy,
			      enum power_supply_property psp,
			      union power_supply_propval *val)
{
	struct p9320 *p = power_supply_get_drvdata(psy);
	int ret = 0;
	u16 v;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = p->attached;
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "P9320";
		return 0;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "IDT";
		return 0;
	default:
		break;
	}

	mutex_lock(&p->lock);
	if (!p->attached) {
		ret = -ENODATA;
		goto out;
	}
	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = p9320_read16(p, P9320_ADC_VOUT, &v);
		val->intval = v * 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = p9320_read16(p, P9320_ADC_IOUT, &v);
		val->intval = v * 1000;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (ret && ret != -EINVAL)
		ret = -ENODATA;
out:
	mutex_unlock(&p->lock);
	return ret;
}

static struct p9320 *p9320_from_dev(struct device *dev)
{
	return power_supply_get_drvdata(to_power_supply(dev));
}

/* a 16-bit reading, only while on a pad */
static ssize_t p9320_show_reg16(struct device *dev, char *buf, u16 reg, u16 mask)
{
	struct p9320 *p = p9320_from_dev(dev);
	ssize_t ret;
	u16 v;

	mutex_lock(&p->lock);
	if (!p->attached)
		ret = -ENODATA;
	else if (p9320_read16(p, reg, &v))
		ret = -EIO;
	else
		ret = sysfs_emit(buf, "%u\n", v & mask);
	mutex_unlock(&p->lock);
	return ret;
}

static ssize_t vrect_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return p9320_show_reg16(dev, buf, P9320_ADC_VRECT, 0xffff);
}
static DEVICE_ATTR_RO(vrect);

static ssize_t freq_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return p9320_show_reg16(dev, buf, P9320_OP_FREQ, 0xffff);
}
static DEVICE_ATTR_RO(freq);

static ssize_t die_temp_raw_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return p9320_show_reg16(dev, buf, P9320_ADC_DIE_TEMP, 0x0fff);
}
static DEVICE_ATTR_RO(die_temp_raw);

static ssize_t vout_set_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct p9320 *p = p9320_from_dev(dev);
	ssize_t ret;
	u8 v;

	mutex_lock(&p->lock);
	if (!p->attached)
		ret = -ENODATA;
	else if (p9320_read(p, P9320_VOUT_SET, &v))
		ret = -EIO;
	else
		ret = sysfs_emit(buf, "%u\n", 3500 + 100 * v);
	mutex_unlock(&p->lock);
	return ret;
}
static DEVICE_ATTR_RO(vout_set);

static ssize_t rx_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct p9320 *p = p9320_from_dev(dev);
	ssize_t ret;
	u16 v;

	mutex_lock(&p->lock);
	if (!p->attached)
		ret = -ENODATA;
	else if (p9320_read16(p, P9320_STATUS_L, &v))
		ret = -EIO;
	else
		ret = sysfs_emit(buf, "status 0x%04x int 0x%04x\n", v, p->last_int);
	mutex_unlock(&p->lock);
	return ret;
}
static DEVICE_ATTR_RO(rx_status);

static ssize_t fw_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct p9320 *p = p9320_from_dev(dev);

	if (!p->attached)
		return -ENODATA;
	return sysfs_emit(buf, "chip 0x%04x rev 0x%02x fw %04x.%04x\n",
			  p->chip_id, p->chip_rev, p->fw_major, p->fw_minor);
}
static DEVICE_ATTR_RO(fw);

static ssize_t tx_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct p9320 *p = p9320_from_dev(dev);

	if (!p->attached || !p->tx_id_done)
		return sysfs_emit(buf, "none\n");
	return sysfs_emit(buf, "0x%02x %s\n", p->tx_id, p9320_tx_class(p->tx_id));
}
static DEVICE_ATTR_RO(tx_id);

static ssize_t pad_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct p9320 *p = p9320_from_dev(dev);
	const char *mode;

	if (!p->attached)
		return sysfs_emit(buf, "none\n");
	switch (p->mode) {
	case P9320_MODE_WPC_BASIC:
		mode = "wpc";
		break;
	case P9320_MODE_WPC_ADV:
		mode = "wpc-adv";
		break;
	case P9320_MODE_PMA_SR1:
	case P9320_MODE_PMA_SR1E:
		mode = "pma";
		break;
	default:
		mode = "unknown";
		break;
	}
	return sysfs_emit(buf, "%s\n", mode);
}
static DEVICE_ATTR_RO(pad);

static ssize_t tx_error_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0x%02x\n", p9320_from_dev(dev)->tx_err);
}
static DEVICE_ATTR_RO(tx_error);

static ssize_t fod_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", p9320_fod_names[p9320_from_dev(dev)->fod]);
}
static DEVICE_ATTR_RO(fod);

static ssize_t hv_capable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", p9320_from_dev(dev)->hv_capable);
}
static DEVICE_ATTR_RO(hv_capable);

static ssize_t hv_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", p9320_hv_names[p9320_from_dev(dev)->hv]);
}
static DEVICE_ATTR_RO(hv_state);

static ssize_t hv_enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", p9320_from_dev(dev)->hv_enable);
}

static ssize_t hv_enable_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct p9320 *p = p9320_from_dev(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;
	mutex_lock(&p->lock);
	if (p->hv_enable != on) {
		p->hv_enable = on;
		dev_info(p->dev, "hv_enable %d\n", on);
		if (!on)
			p9320_start_stepdown(p, "hv_enable 0");
		else if (p->attached)
			p9320_start_hv(p);
	}
	mutex_unlock(&p->lock);
	return count;
}
static DEVICE_ATTR_RW(hv_enable);

static struct attribute *p9320_attrs[] = {
	&dev_attr_vrect.attr,
	&dev_attr_freq.attr,
	&dev_attr_die_temp_raw.attr,
	&dev_attr_vout_set.attr,
	&dev_attr_rx_status.attr,
	&dev_attr_fw.attr,
	&dev_attr_tx_id.attr,
	&dev_attr_pad.attr,
	&dev_attr_tx_error.attr,
	&dev_attr_fod.attr,
	&dev_attr_hv_capable.attr,
	&dev_attr_hv_state.attr,
	&dev_attr_hv_enable.attr,
	NULL
};
ATTRIBUTE_GROUPS(p9320);

static const struct power_supply_desc p9320_desc = {
	.name = "p9320-wireless",
	.type = POWER_SUPPLY_TYPE_WIRELESS,
	.properties = p9320_props,
	.num_properties = ARRAY_SIZE(p9320_props),
	.get_property = p9320_get_property,
};

/* ---- probe ---- */

static bool p9320_read_fod(struct device *dev, const char *prop, u8 *fod)
{
	int n = of_property_count_u8_elems(dev->of_node, prop);

	if (n < 0)
		return false;
	if (n != P9320_FOD_LEN) {
		dev_warn(dev, "%s: %d bytes, want %d; not used\n", prop, n, P9320_FOD_LEN);
		return false;
	}
	return !of_property_read_u8_array(dev->of_node, prop, fod, P9320_FOD_LEN);
}

static void p9320_stop(void *data)
{
	struct p9320 *p = data;

	mutex_lock(&p->lock);
	p->stopping = true;
	mutex_unlock(&p->lock);
	cancel_delayed_work_sync(&p->attach_work);
	cancel_delayed_work_sync(&p->txid_work);
	cancel_delayed_work_sync(&p->txdata_work);
	cancel_delayed_work_sync(&p->hv_work);
	cancel_delayed_work_sync(&p->step_work);
	cancel_delayed_work_sync(&p->monitor_work);
	destroy_workqueue(p->wq);
}

/*
 * The WCIN supply changed (e.g. the charger's OTG 5 V lock came on): look
 * at once rather than at the next 10 s monitor tick, so 10 V steps down.
 */
static int p9320_psy_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct p9320 *p = container_of(nb, struct p9320, psy_nb);
	struct power_supply *psy = data;

	if (event != PSY_EVENT_PROP_CHANGED || !psy->desc->name ||
	    strcmp(psy->desc->name, p->wcin_name))
		return NOTIFY_DONE;
	if (READ_ONCE(p->attached) && READ_ONCE(p->hv) != P9320_HV_OFF)
		p9320_queue(p, &p->monitor_work, 0);
	return NOTIFY_OK;
}

static void p9320_psy_nb_remove(void *data)
{
	struct p9320 *p = data;

	power_supply_unreg_notifier(&p->psy_nb);
}

static int p9320_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct power_supply_config cfg = {};
	struct p9320 *p;
	u32 rpp = 0x40;		/* stock wc_hv_rpp default (no LED cover) */
	int ret;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	p->client = client;
	mutex_init(&p->lock);
	p9320_reset_state(p);
	i2c_set_clientdata(client, p);

	p->have_fod_cc = p9320_read_fod(dev, "idt,fod-cc", p->fod_cc);
	p->have_fod_cv = p9320_read_fod(dev, "idt,fod-cv", p->fod_cv);
	p->have_fod_stand = p9320_read_fod(dev, "idt,fod-stand-5v", p->fod_stand);
	of_property_read_u32(dev->of_node, "idt,rpp-scale", &rpp);
	p->rpp = rpp;
	p->battery_name = "max170xx_battery";
	p->wcin_name = "max77705-wireless";
	p->wired_name = "max77705-charger";
	of_property_read_string(dev->of_node, "zt,battery-name", &p->battery_name);
	of_property_read_string(dev->of_node, "zt,wcin-supply-name", &p->wcin_name);
	of_property_read_string(dev->of_node, "zt,wired-supply-name", &p->wired_name);

	p->det_gpio = devm_gpiod_get(dev, "det", GPIOD_IN);
	if (IS_ERR(p->det_gpio))
		return dev_err_probe(dev, PTR_ERR(p->det_gpio), "det gpio\n");
	p->int_gpio = devm_gpiod_get(dev, "int", GPIOD_IN);
	if (IS_ERR(p->int_gpio))
		return dev_err_probe(dev, PTR_ERR(p->int_gpio), "int gpio\n");
	p->det_irq = gpiod_to_irq(p->det_gpio);
	if (p->det_irq < 0)
		return dev_err_probe(dev, p->det_irq, "det irq\n");
	p->int_irq = gpiod_to_irq(p->int_gpio);
	if (p->int_irq < 0)
		return dev_err_probe(dev, p->int_irq, "int irq\n");

	cfg.drv_data = p;
	cfg.fwnode = dev_fwnode(dev);
	cfg.attr_grp = p9320_groups;
	p->psy = devm_power_supply_register(dev, &p9320_desc, &cfg);
	if (IS_ERR(p->psy))
		return dev_err_probe(dev, PTR_ERR(p->psy), "power supply\n");

	p->wq = alloc_ordered_workqueue("p9320", 0);
	if (!p->wq)
		return -ENOMEM;
	INIT_DELAYED_WORK(&p->attach_work, p9320_attach_work);
	INIT_DELAYED_WORK(&p->txid_work, p9320_txid_work);
	INIT_DELAYED_WORK(&p->txdata_work, p9320_txdata_work);
	INIT_DELAYED_WORK(&p->hv_work, p9320_hv_work);
	INIT_DELAYED_WORK(&p->step_work, p9320_step_work);
	INIT_DELAYED_WORK(&p->monitor_work, p9320_monitor_work);
	ret = devm_add_action_or_reset(dev, p9320_stop, p);
	if (ret)
		return ret;
	p->psy_nb.notifier_call = p9320_psy_event;
	ret = power_supply_reg_notifier(&p->psy_nb);
	if (ret)
		return dev_err_probe(dev, ret, "power supply notifier\n");
	ret = devm_add_action_or_reset(dev, p9320_psy_nb_remove, p);
	if (ret)
		return ret;

	/* INT_A only while on a pad: off it the line has no driver */
	ret = devm_request_threaded_irq(dev, p->int_irq, NULL, p9320_int_irq,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT | IRQF_NO_AUTOEN,
					"p9320-int", p);
	if (ret)
		return dev_err_probe(dev, ret, "int irq %d\n", p->int_irq);
	ret = devm_request_threaded_irq(dev, p->det_irq, NULL, p9320_det_irq,
					IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					"p9320-det", p);
	if (ret)
		return dev_err_probe(dev, ret, "det irq %d\n", p->det_irq);

	device_init_wakeup(dev, true);
	/* already on a pad at boot; the chip is not touched otherwise */
	p9320_queue(p, &p->attach_work, 0);
	dev_info(dev, "probed (det %d, FOD %s%s%s)\n", p9320_det(p),
		 p->have_fod_cc ? "cc " : "", p->have_fod_cv ? "cv " : "",
		 p->have_fod_stand ? "stand-5v" : "");
	return 0;
}

/* Never leave the receiver at 10 V for the next kernel. */
static void p9320_shutdown(struct i2c_client *client)
{
	struct p9320 *p = i2c_get_clientdata(client);

	mutex_lock(&p->lock);
	p->stopping = true;
	if (p->attached && p->vout_idx != P9320_VOUT_5V)
		p9320_write(p, P9320_VOUT_SET, p9320_vout_val[P9320_VOUT_5V]);
	mutex_unlock(&p->lock);
}

static int p9320_suspend(struct device *dev)
{
	struct p9320 *p = dev_get_drvdata(dev);

	if (device_may_wakeup(dev)) {
		enable_irq_wake(p->det_irq);
		if (p->int_enabled)
			enable_irq_wake(p->int_irq);
	}
	return 0;
}

static int p9320_resume(struct device *dev)
{
	struct p9320 *p = dev_get_drvdata(dev);

	if (device_may_wakeup(dev)) {
		disable_irq_wake(p->det_irq);
		if (p->int_enabled)
			disable_irq_wake(p->int_irq);
	}
	/* an edge may have been missed: look again */
	p9320_queue(p, &p->attach_work, 0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(p9320_pm, p9320_suspend, p9320_resume);

static const struct of_device_id p9320_of_match[] = {
	{ .compatible = "idt,p9320" },
	{ }
};
MODULE_DEVICE_TABLE(of, p9320_of_match);

static const struct i2c_device_id p9320_id[] = {
	{ "p9320" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, p9320_id);

static struct i2c_driver p9320_driver = {
	.driver = {
		.name = "p9320-wireless",
		.of_match_table = p9320_of_match,
		.pm = pm_sleep_ptr(&p9320_pm),
	},
	.probe = p9320_probe,
	.shutdown = p9320_shutdown,
	.id_table = p9320_id,
};
module_i2c_driver(p9320_driver);

MODULE_DESCRIPTION("IDT P9320 wireless power receiver");
MODULE_LICENSE("GPL");
