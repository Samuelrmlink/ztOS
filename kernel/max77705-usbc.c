// SPDX-License-Identifier: GPL-2.0
/*
 * MAX77705 USB-C controller ("USBC", Samsung's CCIC/PDIC/MUIC) for ztOS.
 *
 * The MAX77705 carries a small MCU that runs Type-C attach, BC1.2 charger
 * detection, AFC/QC2 and the whole USB-PD stack in its own firmware. The AP
 * talks to it at I2C 0x25:
 *
 *   0x00/0x01/0x09  HW_REV / FW_REV / FW_MINOR
 *   0x02..0x05      UIC/CC/PD/VDM interrupt flags, CLEARED BY READING
 *   0x06..0x0d      status (VBADC, SYSMsg, BC_STATUS, CC_STATUS0/1, PD_STATUS0/1)
 *   0x0e..0x11      interrupt masks (1 = masked)
 *   0x21..0x41      mailbox out: opcode, data; a 0 written to 0x41 ends it
 *   0x51..          mailbox in: echoed opcode, answer
 *
 * The MCU raises the MAX77705's one interrupt line, which the mainline MFD
 * demultiplexes; we take hwirq 3 (MAX77705_SRC_IRQ_USBC) of its domain.
 *
 * This driver is the only owner of that mailbox. Rules it keeps:
 *   - the INT registers are read in exactly one place (usbc_read_ints());
 *   - one command in flight, 3 s timeout, the echo is checked;
 *   - only opcodes on an allowlist for firmware 0x32 are sent, with their
 *     data checked; anything else is refused before it reaches the bus;
 *   - the MCU is never reset (register 0x80 is never written: Samsung does
 *     that on SYSMsg BOOT_POR, we only re-read the state);
 *   - on anything unexpected (other firmware, a timeout) it logs once and
 *     drops to passive status reporting; it never loops on commands.
 *
 * What ztOS inherits: Samsung's shutdown path (and TWRP's kernel) resets the
 * MCU on the way out, so it comes up with SYSMsg = 0x05 BOOT_POR, all masks
 * open except VDM (0x0e..0x11 = 00 00 00 ff on the phone), and its D+/D-
 * switches in their post-reset state. The switches are only ever set by the
 * AP (CONTROL1, opcode 0x06), so without a CONTROL1 write the USB gadget is
 * not on the connector: the old s9p-max77705-muic driver existed for that.
 */
#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define REG_HW_REV		0x00
#define REG_FW_REV		0x01
#define REG_UIC_INT		0x02	/* clear-on-read */
#define  UIC_APCMDRES		BIT(7)
#define  UIC_SYSMSG		BIT(6)
#define  UIC_VBUSDET		BIT(5)
#define  UIC_VBADC		BIT(4)
#define  UIC_DCDTMO		BIT(3)
#define  UIC_CHGTYP		BIT(1)
#define  UIC_UIDADC		BIT(0)
#define REG_CC_INT		0x03	/* clear-on-read */
#define  CC_VCONNOCP		BIT(7)
#define  CC_VSAFE0V		BIT(6)
#define  CC_ATTACHSRCERR	BIT(5)
#define  CC_VCONNSC		BIT(4)
#define  CC_CCPINSTAT		BIT(3)
#define  CC_CCISTAT		BIT(2)
#define  CC_CCVCNSTAT		BIT(1)
#define  CC_CCSTAT		BIT(0)
#define REG_PD_INT		0x04	/* clear-on-read */
#define  PD_PDMSG		BIT(7)
#define  PD_PSRDY		BIT(6)
#define  PD_DATAROLE		BIT(5)
#define  PD_DP_ATTENTION	BIT(4)
#define  PD_DP_CONFIGURE	BIT(3)
#define  PD_DP_STATUS		BIT(2)
#define  PD_SSACC		BIT(1)
#define  PD_FCTID		BIT(0)
#define REG_VDM_INT		0x05	/* clear-on-read */
#define REG_USBC_STATUS1	0x06
#define  STATUS1_VBADC		GENMASK(7, 4)
#define  STATUS1_UIDADC		GENMASK(2, 0)
#define REG_USBC_STATUS2	0x07	/* SYSMsg */
#define REG_BC_STATUS		0x08
#define  BC_VBUSDET		BIT(7)
#define  BC_PRCHGTYP		GENMASK(5, 3)
#define  BC_DCDTMO		BIT(2)
#define  BC_CHGTYP		GENMASK(1, 0)
#define REG_FW_MINOR		0x09
#define REG_CC_STATUS0		0x0a
#define  CC0_CCPINSTAT		GENMASK(7, 6)
#define  CC0_CCISTAT		GENMASK(5, 4)
#define  CC0_CCVCNSTAT		BIT(3)
#define  CC0_CCSTAT		GENMASK(2, 0)
#define REG_CC_STATUS1		0x0b
#define REG_PD_STATUS0		0x0c	/* last PD event code */
#define REG_PD_STATUS1		0x0d
#define  PD1_DATAROLE		BIT(7)
#define  PD1_PSRDY		BIT(4)
#define  PD1_FCTID		GENMASK(3, 0)
#define REG_UIC_INT_M		0x0e
#define REG_CC_INT_M		0x0f
#define REG_PD_INT_M		0x10
#define REG_VDM_INT_M		0x11
#define REG_AP_DATAOUT0		0x21
#define REG_AP_DATAOUT32	0x41
#define REG_AP_DATAIN0		0x51

/* Samsung's final masks (max77705.h *_INT_M_INIT, then usbc probe for PD/VDM) */
#define UIC_INT_M_INIT		0x04
#define CC_INT_M_INIT		0x20	/* AttachSrcErr */
#define PD_INT_M_INIT		0x1c	/* the three DP alt-mode bits */
#define VDM_INT_M_INIT		0xff

#define STATUS_FIRST		REG_USBC_STATUS1
#define STATUS_N		8	/* 0x06..0x0d */
#define ST(reg)			((reg) - STATUS_FIRST)

/* SYSMsg (USBC_STATUS2) */
#define SYSMSG_NONE		0x00
#define SYSMSG_BOOT_WDT		0x03
#define SYSMSG_BOOT_SWRSTREQ	0x04
#define SYSMSG_BOOT_POR		0x05
#define SYSMSG_APCMD_UNKNOWN	0x31
#define SYSMSG_APCMD_INPROGRESS	0x32
#define SYSMSG_APCMD_FAIL	0x33
#define SYSMSG_CC_5V_SHORT	0x61
#define SYSMSG_SBU_GND_SHORT	0x62
#define SYSMSG_SBU_5V_SHORT	0x63
#define SYSMSG_POWER_NEGO	0x80

/* mailbox opcodes (include/linux/ccic/max77705.h, muic/max77705-muic.h) */
#define OP_CONTROL1_R		0x05
#define OP_CONTROL1_W		0x06

#define MBOX_DATA_MAX		32
#define MBOX_TIMEOUT_MS		3000	/* OPCODE_WAIT_TIMEOUT */
#define FW_REV_KNOWN		0x32	/* the firmware the allowlist is for */

/*
 * CONTROL1: NoBCComp[7] RCPS[6] COMP2SW(D+)[5:3] COMN1SW(D-)[2:0];
 * 001 = USB to the AP, 111 = open.
 */
#define COM_USB			0x09
#define COM_OPEN		0x3f

#define POLL_MS			1000	/* only without an interrupt */

#define LOG_N			256
#define LOG_DATA		33

enum log_kind { LOG_IRQ, LOG_SYSMSG, LOG_PDMSG, LOG_CMD, LOG_NOTE };

struct log_entry {
	ktime_t t;
	u8 kind;
	u8 op;		/* LOG_CMD: opcode; else the code */
	u8 wlen, rlen;
	s16 ret;
	u8 w[LOG_DATA];
	u8 r[LOG_DATA];
	const char *note;
};

struct max77705_usbc {
	struct device *dev;
	struct i2c_client *client;
	int irq;

	struct workqueue_struct *wq;	/* ordered: all state and commands */
	struct work_struct event_work;
	struct delayed_work poll_work;

	/* written by the interrupt thread, taken by event_work */
	spinlock_t ev_lock;
	u8 pend[4];			/* UIC, CC, PD, VDM flags since last run */
	u8 sysmsg;			/* latest SYSMsg read on SYSMsgI */
	bool sysmsg_new;
	u8 cmd_op;			/* in flight, 0 = none */
	int cmd_err;
	struct completion cmd_done;

	struct mutex mbox_lock;		/* one command in flight */
	bool mbox_allowed;		/* FW rev is on the allowlist */
	bool mbox_dead;			/* a command timed out: passive from now */
	bool vdm_unmasked;

	u8 hw_rev, fw_rev, fw_minor;
	u8 st[STATUS_N];		/* 0x06..0x0d, last read */

	u8 path;			/* last CONTROL1 written, 0 = none */

	spinlock_t log_lock;
	struct log_entry *log;
	unsigned int log_head, log_count;
	struct dentry *debugfs;
};

/* ---- event log (debugfs) ------------------------------------------------ */

static void usbc_log(struct max77705_usbc *u, enum log_kind kind, u8 op,
		     const u8 *w, int wlen, const u8 *r, int rlen, int ret,
		     const char *note)
{
	struct log_entry *e;
	unsigned long flags;

	if (!u->log)
		return;
	spin_lock_irqsave(&u->log_lock, flags);
	e = &u->log[u->log_head];
	u->log_head = (u->log_head + 1) % LOG_N;
	if (u->log_count < LOG_N)
		u->log_count++;
	memset(e, 0, sizeof(*e));
	e->t = ktime_get_boottime();
	e->kind = kind;
	e->op = op;
	e->ret = ret;
	e->note = note;
	e->wlen = min(wlen, LOG_DATA);
	e->rlen = min(rlen, LOG_DATA);
	if (w && e->wlen)
		memcpy(e->w, w, e->wlen);
	if (r && e->rlen)
		memcpy(e->r, r, e->rlen);
	spin_unlock_irqrestore(&u->log_lock, flags);
}

static const char *sysmsg_name(u8 c)
{
	switch (c) {
	case SYSMSG_NONE: return "none";
	case SYSMSG_BOOT_WDT: return "BOOT_WDT";
	case SYSMSG_BOOT_SWRSTREQ: return "BOOT_SWRSTREQ";
	case SYSMSG_BOOT_POR: return "BOOT_POR";
	case 0x10: return "HV_NOVBUS";
	case 0x11: return "HV_RXPERR";
	case 0x12: return "HV_RXBUFOW";
	case 0x13: return "HV_RXTFR";
	case 0x14: return "HV_MPNACK";
	case 0x15: return "HV_RESET_FAIL";
	case 0x20: return "AFC_DONE";
	case 0x30: return "SYSPOS";
	case SYSMSG_APCMD_UNKNOWN: return "APCMD_UNKNOWN";
	case SYSMSG_APCMD_INPROGRESS: return "APCMD_INPROGRESS";
	case SYSMSG_APCMD_FAIL: return "APCMD_FAIL";
	case SYSMSG_CC_5V_SHORT: return "CC_VBUS_SHORT";
	case SYSMSG_SBU_GND_SHORT: return "SBU_GND_SHORT";
	case SYSMSG_SBU_5V_SHORT: return "SBU_VBUS_SHORT";
	case 0x64: return "SET_GRL";
	case 0x70: return "FACTORY_RID0";
	case SYSMSG_POWER_NEGO: return "POWER_NEGO_ERR";
	default: return "?";
	}
}

static const char *pdmsg_name(u8 c)
{
	switch (c) {
	case 0x00: return "nothing";
	case 0x01: return "SNK_PSRDY_RECEIVED";
	case 0x02: return "SNK_ERROR_RECOVERY";
	case 0x03: return "SNK_SENDER_RESPONSE_TIMEOUT";
	case 0x04: return "SRC_PSRDY_SENT";
	case 0x05: return "SRC_ERROR_RECOVERY";
	case 0x06: return "SRC_SENDER_RESPONSE_TIMEOUT";
	case 0x07: return "DR_SWAP_REQ_RECEIVED";
	case 0x08: return "PR_SWAP_REQ_RECEIVED";
	case 0x09: return "VCONN_SWAP_REQ_RECEIVED";
	case 0x0a: return "MSG_IN_ILLEGAL_STATE";
	case 0x10: return "SAMSUNG_ACCESSORY";
	case 0x11: return "VDM_ATTENTION";
	case 0x12: return "REJECT_RECEIVED";
	case 0x13: return "NOT_SUPPORTED_RECEIVED";
	case 0x14: return "PRSWAP_SNKTOSRC_SENT";
	case 0x15: return "PRSWAP_SRCTOSNK_SENT";
	case 0x16: return "HARDRESET_RECEIVED";
	case 0x17: return "VBUS_TURN_ON";
	case 0x18: return "VBUS_TURN_OFF";
	case 0x19: return "HARDRESET_SENT";
	case 0x1a: return "PRSWAP_SRCTOSWAP";
	case 0x1b: return "PRSWAP_SWAPTOSNK";
	case 0x1c: return "PRSWAP_SNKTOSWAP";
	case 0x1d: return "PRSWAP_SWAPTOSRC";
	case 0x20: return "SNK_PD_DISABLED";
	case 0x21: return "SRC_PD_DISABLED";
	case 0x22: return "CURRENT_CABLE";
	case 0x30: return "GET_SRC_CAP_EXT_RECEIVED";
	case 0x31: return "GET_STATUS_RECEIVED";
	case 0x32: return "GET_BATTERY_CAP_RECEIVED";
	case 0x33: return "GET_BATTERY_STATUS_RECEIVED";
	case 0x34: return "GET_MANUFACTURER_INFO_RECEIVED";
	case 0x35: return "SRC_CAP_EXT_RECEIVED";
	case 0x36: return "STATUS_RECEIVED";
	case 0x37: return "BATTERY_CAP_RECEIVED";
	case 0x38: return "BATTERY_STATUS_RECEIVED";
	case 0x39: return "MANUFACTURER_INFO_RECEIVED";
	case 0x40: return "VDM_NAK";
	case 0x41: return "VDM_BUSY";
	case 0x42: return "VDM_ACK";
	case 0x43: return "VDM_REQ";
	case 0x50: return "AFC_SNK_PD_CAPS";
	case 0x51: return "AFC_SNK_PD_PSRDY";
	case 0x52: return "AFC_VDM_RESPONSE_TIMEOUT";
	case 0x61: return "DP_DISCOVER_IDENTITY";
	case 0x62: return "DP_DISCOVER_SVID";
	case 0x63: return "DP_DISCOVER_MODES";
	case 0x64: return "DP_ENTER_MODE";
	case 0x65: return "DP_EXIT_MODE";
	case 0x66: return "DP_STATUS";
	case 0x67: return "DP_CONFIGURE";
	case 0x68: return "DP_ATTENTION";
	default: return "?";
	}
}

/* ---- registers ---------------------------------------------------------- */

static int usbc_read(struct max77705_usbc *u, u8 reg, u8 *buf, int len)
{
	struct i2c_msg msg[2] = {
		{ .addr = u->client->addr, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = u->client->addr, .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	int ret;

	ret = i2c_transfer(u->client->adapter, msg, 2);
	if (ret == 2)
		return 0;
	return ret < 0 ? ret : -EIO;
}

/*
 * The only registers this driver ever writes: the four interrupt masks and
 * the mailbox. Anything else is refused here, so no code path can reach
 * 0x80 (MCU reset) or another block.
 */
static int usbc_write(struct max77705_usbc *u, u8 reg, const u8 *buf, int len)
{
	u8 b[1 + MBOX_DATA_MAX + 1];
	bool ok_mask = reg >= REG_UIC_INT_M && reg + len - 1 <= REG_VDM_INT_M;
	bool ok_mbox = reg >= REG_AP_DATAOUT0 && reg + len - 1 <= REG_AP_DATAOUT32;
	int ret;

	if (len < 1 || len > MBOX_DATA_MAX + 1 || !(ok_mask || ok_mbox)) {
		dev_err(u->dev, "refused write to %02x+%d\n", reg, len);
		return -EPERM;
	}
	b[0] = reg;
	memcpy(b + 1, buf, len);
	ret = i2c_master_send(u->client, b, len + 1);
	if (ret == len + 1)
		return 0;
	return ret < 0 ? ret : -EIO;
}

static int usbc_write_byte(struct max77705_usbc *u, u8 reg, u8 val)
{
	return usbc_write(u, reg, &val, 1);
}

static int usbc_read_status(struct max77705_usbc *u)
{
	u8 st[STATUS_N];
	int ret = usbc_read(u, STATUS_FIRST, st, STATUS_N);

	if (!ret)
		memcpy(u->st, st, STATUS_N);
	return ret;
}

/*
 * Read (and so clear) the interrupt flags: the one place that does. UIC, CC
 * and PD in one block as Samsung does on PASS3; VDM only while unmasked
 * (Samsung reads it only then). Called from the interrupt thread, or from
 * the poll work when there is no interrupt.
 */
static void usbc_read_ints(struct max77705_usbc *u)
{
	u8 f[4] = {};
	u8 sysmsg = 0;
	unsigned long flags;
	int i, ret;

	ret = usbc_read(u, REG_UIC_INT, f, 3);
	if (ret) {
		dev_err_ratelimited(u->dev, "INT read failed: %d\n", ret);
		return;
	}
	if (u->vdm_unmasked && usbc_read(u, REG_VDM_INT, &f[3], 1))
		f[3] = 0;
	if (!(f[0] | f[1] | f[2] | f[3]))
		return;

	if (f[0] & UIC_SYSMSG) {
		/* Samsung reads it three times and trusts it when they agree */
		u8 s[3] = {};

		for (i = 0; i < 3; i++)
			usbc_read(u, REG_USBC_STATUS2, &s[i], 1);
		sysmsg = (s[0] == s[1] && s[1] == s[2]) ? s[0] : s[2];
	}

	spin_lock_irqsave(&u->ev_lock, flags);
	for (i = 0; i < 4; i++)
		u->pend[i] |= f[i];
	if (f[0] & UIC_SYSMSG) {
		u->sysmsg = sysmsg;
		u->sysmsg_new = true;
		/* a command error answers the command in flight */
		if (u->cmd_op && sysmsg >= SYSMSG_APCMD_UNKNOWN &&
		    sysmsg <= SYSMSG_APCMD_FAIL) {
			u->cmd_err = sysmsg == SYSMSG_APCMD_UNKNOWN ? -EOPNOTSUPP :
				     sysmsg == SYSMSG_APCMD_INPROGRESS ? -EBUSY : -EIO;
			complete(&u->cmd_done);
		}
	}
	if ((f[0] & UIC_APCMDRES) && u->cmd_op)
		complete(&u->cmd_done);
	spin_unlock_irqrestore(&u->ev_lock, flags);

	usbc_log(u, LOG_IRQ, 0, f, 4, NULL, 0, 0, NULL);
	if (f[0] & UIC_SYSMSG)
		usbc_log(u, LOG_SYSMSG, sysmsg, NULL, 0, NULL, 0, 0,
			 sysmsg_name(sysmsg));
	queue_work(u->wq, &u->event_work);
}

static irqreturn_t usbc_irq(int irq, void *data)
{
	usbc_read_ints(data);
	return IRQ_HANDLED;
}

static int usbc_set_masks(struct max77705_usbc *u)
{
	u8 m[4] = { UIC_INT_M_INIT, CC_INT_M_INIT, PD_INT_M_INIT, VDM_INT_M_INIT };
	int ret = usbc_write(u, REG_UIC_INT_M, m, 4);

	if (!ret)
		u->vdm_unmasked = m[3] != 0xff;
	return ret;
}

/* ---- mailbox ------------------------------------------------------------ */

/*
 * The allowlist, for FW_REV 0x32 (the image in Samsung's S9 tree is 0x32.00
 * too, so these are the opcodes and data stock sends to this firmware).
 * Data is checked as well as the opcode. Never on it, by design: 0xd0
 * (firmware update), 0x50 (key revocation), 0xd1 (RAM BIST), 0x74 (debug),
 * 0x70 (GRL/FW clear), 0x1b, 0x20, 0x33, 0x54, 0x55, 0x16/0x18 and every
 * other write whose effect is not known.
 */
static bool usbc_op_allowed(struct max77705_usbc *u, u8 op, const u8 *w, int wlen)
{
	switch (op) {
	case OP_CONTROL1_R:
		return wlen == 0;
	case OP_CONTROL1_W:
		return wlen == 1 && (w[0] == COM_USB || w[0] == COM_OPEN);
	default:
		return false;
	}
}

/*
 * Send one command and wait for its answer: [op, w...] at 0x21, end marker
 * at 0x41, APCmdResI, then rlen+1 bytes from 0x51 whose first byte must echo
 * op. Returns 0 or a negative error; r gets rlen answer bytes (after the
 * echo). Must not be called from the interrupt thread: it waits on it.
 */
static int usbc_cmd(struct max77705_usbc *u, u8 op, const u8 *w, int wlen,
		    u8 *r, int rlen)
{
	u8 out[1 + MBOX_DATA_MAX], in[1 + MBOX_DATA_MAX] = {};
	int inlen = max(rlen, 1) + 1;
	unsigned long flags, left;
	int ret, err;

	if (wlen < 0 || wlen > MBOX_DATA_MAX || rlen < 0 || rlen > MBOX_DATA_MAX)
		return -EINVAL;
	if (!u->mbox_allowed)
		return -EPERM;	/* said at probe */
	if (!usbc_op_allowed(u, op, w, wlen)) {
		dev_warn_ratelimited(u->dev, "opcode %02x refused (fw %02x)\n",
				     op, u->fw_rev);
		usbc_log(u, LOG_CMD, op, w, wlen, NULL, 0, -EPERM, "refused");
		return -EPERM;
	}
	if (u->mbox_dead)
		return -ENODEV;

	mutex_lock(&u->mbox_lock);
	spin_lock_irqsave(&u->ev_lock, flags);
	reinit_completion(&u->cmd_done);
	u->cmd_op = op;
	u->cmd_err = 0;
	spin_unlock_irqrestore(&u->ev_lock, flags);

	out[0] = op;
	if (wlen)
		memcpy(out + 1, w, wlen);
	ret = usbc_write(u, REG_AP_DATAOUT0, out, wlen + 1);
	if (!ret && wlen < MBOX_DATA_MAX)
		ret = usbc_write_byte(u, REG_AP_DATAOUT32, 0);
	if (ret)
		goto out;

	if (u->irq > 0) {
		left = wait_for_completion_timeout(&u->cmd_done,
						   msecs_to_jiffies(MBOX_TIMEOUT_MS));
	} else {
		/* no interrupt: poll the flags (we are their only reader) */
		unsigned long end = jiffies + msecs_to_jiffies(MBOX_TIMEOUT_MS);

		do {
			usleep_range(5000, 10000);
			usbc_read_ints(u);
		} while (!completion_done(&u->cmd_done) && time_before(jiffies, end));
		left = completion_done(&u->cmd_done);
	}

	spin_lock_irqsave(&u->ev_lock, flags);
	err = u->cmd_err;
	spin_unlock_irqrestore(&u->ev_lock, flags);

	if (!left) {
		ret = -ETIMEDOUT;
		u->mbox_dead = true;
		dev_err(u->dev,
			"opcode %02x: no answer in %d ms; mailbox off, passive status only from now\n",
			op, MBOX_TIMEOUT_MS);
		goto out;
	}
	if (err) {
		ret = err;
		dev_warn(u->dev, "opcode %02x: MCU says %s\n", op,
			 sysmsg_name(u->sysmsg));
		goto out;
	}
	ret = usbc_read(u, REG_AP_DATAIN0, in, inlen);
	if (ret)
		goto out;
	if (in[0] != op) {
		dev_warn(u->dev, "opcode %02x: answer echoes %02x\n", op, in[0]);
		ret = -EPROTO;
		goto out;
	}
	if (r && rlen)
		memcpy(r, in + 1, rlen);
out:
	spin_lock_irqsave(&u->ev_lock, flags);
	u->cmd_op = 0;
	spin_unlock_irqrestore(&u->ev_lock, flags);
	mutex_unlock(&u->mbox_lock);
	usbc_log(u, LOG_CMD, op, out, wlen + 1, in, inlen, ret, NULL);
	return ret;
}

/* ---- D+/D- switch --------------------------------------------------------- */

static int usbc_set_path(struct max77705_usbc *u, u8 val)
{
	int ret = usbc_cmd(u, OP_CONTROL1_W, &val, 1, NULL, 0);

	if (ret) {
		/* -EPERM/-ENODEV: passive mode, said once already */
		if (ret != -EPERM && ret != -ENODEV)
			dev_err(u->dev, "CONTROL1 <= %02x failed: %d\n", val, ret);
		return ret;
	}
	u->path = val;
	dev_info(u->dev, "D+/D- %s (CONTROL1 %02x)\n",
		 val == COM_USB ? "to the AP" : "open", val);
	return 0;
}

/* ---- events --------------------------------------------------------------- */

/* SYSMsg: log it; after an MCU (re)boot re-apply the masks and forget what
 * we had told it. Never reset the MCU ourselves. */
static void usbc_handle_sysmsg(struct max77705_usbc *u, u8 msg)
{
	switch (msg) {
	case SYSMSG_BOOT_POR:
	case SYSMSG_BOOT_WDT:
	case SYSMSG_BOOT_SWRSTREQ:
		dev_info(u->dev, "MCU rebooted (SYSMsg %02x %s): masks and switch again\n",
			 msg, sysmsg_name(msg));
		usbc_set_masks(u);
		u->path = 0;
		break;
	case SYSMSG_CC_5V_SHORT:
	case SYSMSG_SBU_GND_SHORT:
	case SYSMSG_SBU_5V_SHORT:
		dev_warn(u->dev, "SYSMsg %02x %s\n", msg, sysmsg_name(msg));
		break;
	default:
		dev_dbg(u->dev, "SYSMsg %02x %s\n", msg, sysmsg_name(msg));
		break;
	}
}

static void usbc_event_work(struct work_struct *work)
{
	struct max77705_usbc *u = container_of(work, struct max77705_usbc, event_work);
	u8 pend[4], sysmsg;
	bool sysmsg_new, vbus, was_vbus;
	unsigned long flags;

	spin_lock_irqsave(&u->ev_lock, flags);
	memcpy(pend, u->pend, sizeof(pend));
	memset(u->pend, 0, sizeof(u->pend));
	sysmsg = u->sysmsg;
	sysmsg_new = u->sysmsg_new;
	u->sysmsg_new = false;
	spin_unlock_irqrestore(&u->ev_lock, flags);

	if (sysmsg_new)
		usbc_handle_sysmsg(u, sysmsg);

	was_vbus = u->st[ST(REG_BC_STATUS)] & BC_VBUSDET;
	if (usbc_read_status(u))
		return;
	vbus = u->st[ST(REG_BC_STATUS)] & BC_VBUSDET;

	if (pend[2] & PD_PDMSG) {
		u8 code = u->st[ST(REG_PD_STATUS0)];

		usbc_log(u, LOG_PDMSG, code, NULL, 0, NULL, 0, 0, pdmsg_name(code));
	}

	/* as the old driver did: D+/D- to the AP at start, after an MCU
	 * reboot, and whenever VBUS comes back (the MCU may have dropped it) */
	if (!u->path || (vbus && !was_vbus))
		usbc_set_path(u, COM_USB);
}

static void usbc_poll_work(struct work_struct *work)
{
	struct max77705_usbc *u = container_of(to_delayed_work(work),
					       struct max77705_usbc, poll_work);

	usbc_read_ints(u);
	queue_delayed_work(u->wq, &u->poll_work, msecs_to_jiffies(POLL_MS));
}

/* ---- debugfs -------------------------------------------------------------- */

static int usbc_events_show(struct seq_file *s, void *unused)
{
	struct max77705_usbc *u = s->private;
	struct log_entry *copy;
	unsigned int n, first, i, j;
	unsigned long flags;

	copy = kcalloc(LOG_N, sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	spin_lock_irqsave(&u->log_lock, flags);
	n = u->log_count;
	first = (u->log_head + LOG_N - n) % LOG_N;
	for (i = 0; i < n; i++)
		copy[i] = u->log[(first + i) % LOG_N];
	spin_unlock_irqrestore(&u->log_lock, flags);

	seq_puts(s, "# firmware events, not a PD sniffer: the MCU runs PD and reports summaries\n");
	for (i = 0; i < n; i++) {
		struct log_entry *e = &copy[i];
		s64 us = ktime_to_us(e->t);

		seq_printf(s, "%5lld.%06lld ", us / USEC_PER_SEC, us % USEC_PER_SEC);
		switch (e->kind) {
		case LOG_IRQ:
			seq_printf(s, "irq uic=%02x cc=%02x pd=%02x vdm=%02x\n",
				   e->w[0], e->w[1], e->w[2], e->w[3]);
			break;
		case LOG_SYSMSG:
			seq_printf(s, "sysmsg %02x %s\n", e->op, e->note);
			break;
		case LOG_PDMSG:
			seq_printf(s, "pd_event %02x %s\n", e->op, e->note);
			break;
		case LOG_CMD:
			seq_printf(s, "cmd %02x ret=%d%s%s out=", e->op, e->ret,
				   e->note ? " " : "", e->note ?: "");
			for (j = 0; j < e->wlen; j++)
				seq_printf(s, "%02x", e->w[j]);
			seq_puts(s, " in=");
			for (j = 0; j < e->rlen; j++)
				seq_printf(s, "%02x", e->r[j]);
			seq_putc(s, '\n');
			break;
		default:
			seq_printf(s, "note %s %02x\n", e->note ?: "", e->op);
			break;
		}
	}
	kfree(copy);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(usbc_events);

/* status and masks only: never 0x02..0x05 (clear-on-read) or 0x51.. */
static int usbc_regs_show(struct seq_file *s, void *unused)
{
	static const u8 regs[] = { 0x00, 0x01, 0x06, 0x07, 0x08, 0x09, 0x0a,
				   0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11 };
	struct max77705_usbc *u = s->private;
	unsigned int i;
	u8 v;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		if (usbc_read(u, regs[i], &v, 1))
			seq_printf(s, "%02x: err\n", regs[i]);
		else
			seq_printf(s, "%02x: %02x\n", regs[i], v);
	}
	seq_printf(s, "mailbox: %s\n", !u->mbox_allowed ? "off (firmware not on the allowlist)" :
		   u->mbox_dead ? "off (timed out)" : "on");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(usbc_regs);

static void usbc_debugfs_init(struct max77705_usbc *u)
{
	u->debugfs = debugfs_create_dir("max77705-usbc", NULL);
	debugfs_create_file("events", 0400, u->debugfs, u, &usbc_events_fops);
	debugfs_create_file("registers", 0400, u->debugfs, u, &usbc_regs_fops);
}

/* ---- probe ---------------------------------------------------------------- */

static void usbc_teardown(void *data)
{
	struct max77705_usbc *u = data;

	debugfs_remove_recursive(u->debugfs);
	cancel_delayed_work_sync(&u->poll_work);
	destroy_workqueue(u->wq);
}

static int max77705_usbc_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct max77705_usbc *u;
	u8 rev[2], minor = 0;
	int ret;

	u = devm_kzalloc(dev, sizeof(*u), GFP_KERNEL);
	if (!u)
		return -ENOMEM;
	u->dev = dev;
	u->client = client;
	u->irq = client->irq;
	spin_lock_init(&u->ev_lock);
	spin_lock_init(&u->log_lock);
	mutex_init(&u->mbox_lock);
	init_completion(&u->cmd_done);
	INIT_WORK(&u->event_work, usbc_event_work);
	INIT_DELAYED_WORK(&u->poll_work, usbc_poll_work);
	i2c_set_clientdata(client, u);
	u->log = devm_kcalloc(dev, LOG_N, sizeof(*u->log), GFP_KERNEL);

	ret = usbc_read(u, REG_HW_REV, rev, 2);
	if (ret)
		return dev_err_probe(dev, ret, "MAX77705 USBC does not answer\n");
	usbc_read(u, REG_FW_MINOR, &minor, 1);
	u->hw_rev = rev[0];
	u->fw_rev = rev[1];
	u->fw_minor = minor;
	u->mbox_allowed = u->fw_rev == FW_REV_KNOWN;
	if (!u->mbox_allowed)
		dev_warn(dev, "firmware %02x.%02x is not %02x: no commands at all, status only\n",
			 u->fw_rev, u->fw_minor, FW_REV_KNOWN);

	ret = usbc_read_status(u);
	if (ret)
		return dev_err_probe(dev, ret, "status read failed\n");
	dev_info(dev, "HW %02x FW %02x.%02x; usbc1=%02x sysmsg=%02x(%s) bc=%02x cc0=%02x cc1=%02x pd0=%02x pd1=%02x\n",
		 u->hw_rev, u->fw_rev, u->fw_minor, u->st[0], u->st[1],
		 sysmsg_name(u->st[1]), u->st[2], u->st[4], u->st[5], u->st[6],
		 u->st[7]);

	u->wq = alloc_ordered_workqueue("max77705-usbc", 0);
	if (!u->wq)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, usbc_teardown, u);
	if (ret)
		return ret;

	{
		u8 m[4];

		if (!usbc_read(u, REG_UIC_INT_M, m, 4))
			dev_info(dev, "masks were %02x %02x %02x %02x\n",
				 m[0], m[1], m[2], m[3]);
	}
	ret = usbc_set_masks(u);
	if (ret)
		return dev_err_probe(dev, ret, "mask write failed\n");

	usbc_debugfs_init(u);

	if (u->irq > 0) {
		ret = devm_request_threaded_irq(dev, u->irq, NULL, usbc_irq,
						IRQF_ONESHOT, "max77705-usbc", u);
		if (ret)
			return dev_err_probe(dev, ret, "IRQ %d\n", u->irq);
	} else {
		dev_warn(dev, "no interrupt: polling every %d ms\n", POLL_MS);
		queue_delayed_work(u->wq, &u->poll_work, msecs_to_jiffies(POLL_MS));
	}

	/* the flags may have been set long before we came: read them once */
	usbc_read_ints(u);
	/* and decide the switch from what is there now */
	queue_work(u->wq, &u->event_work);
	return 0;
}

static const struct of_device_id max77705_usbc_of_match[] = {
	{ .compatible = "maxim,max77705-usbc" },
	{ }
};
MODULE_DEVICE_TABLE(of, max77705_usbc_of_match);

static struct i2c_driver max77705_usbc_driver = {
	.driver = {
		.name		= "max77705-usbc",
		.of_match_table	= max77705_usbc_of_match,
	},
	.probe	= max77705_usbc_probe,
};
module_i2c_driver(max77705_usbc_driver);

MODULE_DESCRIPTION("MAX77705 USB-C controller (Galaxy S9)");
MODULE_LICENSE("GPL");
