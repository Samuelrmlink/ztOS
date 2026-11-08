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
 *
 * Written from Samsung's S9 sources (drivers/ccic/max77705_*.c, muic/
 * max77705-muic*.c, battery_v2/sec_battery.c; GPL-2.0). The design was then
 * checked against the hardware-tested Galaxy Tab S8+ driver in
 * aaronsb/sm-x800-linux (drivers/usb/typec/max77705.c, GPL-2.0), which
 * agrees on the IRQ line, the mailbox and the PSRDY -> 0x30 -> 0x32 order;
 * its firmware (5F.00) takes opcodes the S9's 0x32 does not (0x2e), so its
 * allowlist is not this one.
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
#include <linux/mfd/max77705-private.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/usb/pd.h>
#include <linux/usb/typec.h>
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
#define OP_BC_CTRL2_W		0x04
#define  BC_CTRL2_AFC_RESET	0x13	/* DPDNMan on, D+ to GND, D- open */
#define OP_AFC			0x21	/* COMMAND_AFC_RESULT_READ: one TX byte */
#define  AFC_TX_5V		0x08
#define  AFC_TX_9V		0x46	/* high nibble: 0 = 5 V, 4 = 9 V */
#define  AFC_ANSWER_LEN		10	/* result, VBADC, 8 RX bytes */
#define OP_QC2			0x22	/* COMMAND_QC_2_0_SET: "dpdndrv" */
#define  QC2_5V			0x04
#define  QC2_9V			0x09
#define  QC2_ANSWER_LEN		2	/* result, VBADC */
#define OP_CUR_SRCCAP		0x30	/* pos/n, then n PDOs LE */
#define OP_GET_SRCCAP		0x31
#define OP_SRCCAP_REQUEST	0x32	/* object position, 1-based */
#define  SRCCAP_ANSWER_LEN	31
#define OP_SET_VDM_REQ		0x48	/* [n | 1<<3, VDM header, VDOs]: send to SOP */
#define  VDM_ANSWER_LEN		29	/* [hdr byte, reply VDM header, VDOs] */
#define OP_GET_VDM_RESP		0x4b	/* [sub-id] -> [sub, PD hdr, VDM hdr, VDOs] */
#define  VDM_SUB_IDENTITY	0x01
#define  VDM_SUB_SVIDS		0x02
#define  VDM_SUB_MODES		0x03
#define  VDM_RESP_LEN		31
#define OP_SET_ALTMODE		0x55	/* 3: the firmware runs discovery itself */
#define  ALTMODE_SRC_VDM	0x03
#define OP_CONTROL1_R		0x05
#define OP_CONTROL1_W		0x06
#define OP_FW_AUTOIBUS		0x57	/* OPCODE_SAMSUNG_FW_AUTOIBUS */
#define  AUTOIBUS_AT_OFF	0x01	/* MCU firmware on, automatic IBUS off */

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

/*
 * AutoIBUS: the MCU firmware can set the charger's CHGIN_ILIM itself.
 * Stock turns that off (0x57 = 0x01) at PD init and again whenever the
 * charger's DETAILS_00 bit 7 reads 0 ("ILIM set by USBC"), so that the AP
 * alone owns the input limit. A write to the MCU, so it is a switch.
 */
static bool autoibus_off = true;
module_param(autoibus_off, bool, 0644);
MODULE_PARM_DESC(autoibus_off,
		 "send AutoIBUS off (opcode 0x57 = 0x01) at start and when DETAILS_00 bit 7 is 0, as stock (default 1)");

/*
 * Both off by default; see charging/pd-vdm.md. vdm_discovery: send stock's
 * 0x55 = 3 so the firmware runs Discover Identity/SVIDs/Modes itself, and
 * read the results (partner identity in /sys/class/typec). vdm_debugfs:
 * debugfs max77705-usbc/vdm sends a VDM (0x48) written by root; 1 allows
 * only structured Discover Identity/SVIDs/Modes requests, 2 any VDM.
 */
static bool vdm_discovery;
module_param(vdm_discovery, bool, 0444);
MODULE_PARM_DESC(vdm_discovery,
		 "let the firmware run Discover Identity/SVIDs/Modes (opcode 0x55 = 3) and read the results (default 0)");

static int vdm_debugfs;
module_param(vdm_debugfs, int, 0444);
MODULE_PARM_DESC(vdm_debugfs,
		 "debugfs vdm file: 0 off, 1 Discover Identity/SVIDs/Modes only, 2 any VDM (default 0)");

#define CHG_DETAILS_00_ILIM_BY_CHG	BIT(7)
#define AUTOIBUS_CHECK_MS	2000	/* at most one DETAILS_00 check per */
#define AUTOIBUS_MAX_PER_ATTACH	5	/* resends, then give up until detach */

/* BC_STATUS ChgTyp / PrChgTyp, CC_STATUS0 CCStat / CCIStat */
#define CHGTYP_NONE		0
#define CHGTYP_SDP		1
#define CHGTYP_CDP		2
#define CHGTYP_DCP		3
#define CCSTAT_NONE		0
#define CCSTAT_SINK		1	/* the phone sinks: a source is attached */
#define CCSTAT_SOURCE		2
#define CCI_DEFAULT		1
#define CCI_1_5A		2
#define CCI_3_0A		3

/*
 * The charger types this driver tells apart: the max77705-usbc
 * power_supply's charger_type attribute (names as in the charging contract).
 */
enum usbc_kind {
	K_NONE,		/* no VBUS */
	K_UNKNOWN,	/* VBUS, BC1.2 not (yet) resolved */
	K_SDP,
	K_CDP,
	K_DCP,
	K_DCD_TIMEOUT,
	K_SAMSUNG_2A,	/* PrChgTyp 1..6 */
	K_APPLE_05A,
	K_APPLE_1A,
	K_APPLE_2A,
	K_APPLE_12W,
	K_DCP_3A,
	K_RP_1_5A,	/* Type-C current wins over BC1.2 */
	K_RP_3A,
	K_AFC_9V,
	K_QC2_9V,
	K_PD,
	K_N
};

static const char * const kind_name[K_N] = {
	[K_NONE] = "none",		[K_UNKNOWN] = "unknown",
	[K_SDP] = "sdp",		[K_CDP] = "cdp",
	[K_DCP] = "dcp",		[K_DCD_TIMEOUT] = "dcd-timeout",
	[K_SAMSUNG_2A] = "samsung-2a",	[K_APPLE_05A] = "apple-0.5a",
	[K_APPLE_1A] = "apple-1a",	[K_APPLE_2A] = "apple-2a",
	[K_APPLE_12W] = "apple-12w",	[K_DCP_3A] = "dcp-3a",
	[K_RP_1_5A] = "rp-1.5a",	[K_RP_3A] = "rp-3a",
	[K_AFC_9V] = "afc-9v",		[K_QC2_9V] = "qc2-9v",
	[K_PD] = "pd",
};

/*
 * Input current per type, mA: stock's cable-info table (DT, rev26) via
 * sec_bat_cable_check(): USB 475, CDP 1500, TA (DCP and every "unofficial
 * TA") 1800, DCD timeout 500, 9V TA 1650; Rp 1.5 A 1500, Rp 3.0 A 3000
 * (sec_bat_set_rp_current). Two are below stock on purpose: stock gives
 * Apple 0.5 A and 1 A signatures the TA's 1800 and lets AICL find out.
 */
static const unsigned int kind_ma[K_N] = {
	[K_NONE] = 0,			[K_UNKNOWN] = 500,
	[K_SDP] = 475,			[K_CDP] = 1500,
	[K_DCP] = 1800,			[K_DCD_TIMEOUT] = 500,
	[K_SAMSUNG_2A] = 1800,		[K_APPLE_05A] = 500,
	[K_APPLE_1A] = 1000,		[K_APPLE_2A] = 1800,
	[K_APPLE_12W] = 1800,		[K_DCP_3A] = 1800,
	[K_RP_1_5A] = 1500,		[K_RP_3A] = 3000,
	[K_AFC_9V] = 1650,		[K_QC2_9V] = 1650,
	[K_PD] = 0,			/* from the contract */
};

/* what the power_supply shows; voltage_now is read live from st[] */
struct usbc_pub {
	bool online;
	enum usbc_kind kind;
	enum power_supply_usb_type usb_type;
	unsigned int cur_ua;
	unsigned int vmax_uv;
};

#define PDQ_N			16

enum hv_mode { HV_NONE, HV_AFC_9V, HV_QC2_9V };

#define PD_MAX_PDO		7
struct pd_caps {
	u8 n;
	u32 pdo[PD_MAX_PDO];
};

/* USB-PD as a sink, one CC attach */
struct usbc_pd {
	bool have_first;
	struct pd_caps first;		/* the first caps seen on this attach */
	struct pd_caps cur;		/* the ones in force (latest read) */
	u8 pos;				/* object position in the contract, 0: none */
	u8 req_pos;			/* what we asked for, 0: nothing */
	unsigned int req_tries;
	bool hv_blocked;		/* hard reset after we went above 5 V */

	/* since boot, never reset */
	bool hr_pending;		/* a hard reset since the last caps read */
	struct pd_caps hr_caps;		/* the caps in force at that hard reset */
	unsigned long req_jiffies;	/* our last request (0x32/AFC/QC2) */

	/* since boot, never reset */
	unsigned int hard_resets, hr_sent, hr_received;
	unsigned int hr_no_pd;		/* ours, on a charger without PD: not counted above */
	unsigned int err_recovery, srt_timeouts;
	unsigned int caps_renegotiated;	/* new caps, no hard reset: normal */
	unsigned int caps_changed_after_hr; /* different caps after a hard reset */
	struct pd_caps before, after;	/* the last change of either kind */
	char last_event[200];
	char last_hard_reset[256];
};

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
	u8 path_tried;			/* value written for this attach */

	bool autoibus_sent;		/* since probe / the last MCU reboot */
	unsigned int autoibus_resends;	/* this attach */
	unsigned long autoibus_checked;	/* jiffies */

	/* PD event codes (PD_STATUS0) read in the interrupt thread, in order */
	u8 pdq[PDQ_N];
	unsigned int pdq_n, pdq_lost;

	/* high voltage: all in the ordered work, hv_enable from sysfs */
	struct delayed_work hv_work;
	bool hv_enable;
	bool hv_rearm;			/* hv_enable written 0 -> 1 */
	bool stopping;
	enum hv_mode hv_mode;		/* AFC/QC2 9 V reached */
	bool hv_dcp_done;		/* AFC/QC2 tried on this attach */
	unsigned int hv_prepare_ma;	/* published ceiling while switching */
	unsigned int hv_waits;		/* for the charger's limit to come down */

	struct usbc_pd pd;
	unsigned int pd_gen;		/* bumped whenever pd.cur changes */

	/* Type-C class (CONFIG_TYPEC) */
	struct typec_port *port;
	struct typec_partner *partner;
	struct usb_power_delivery *partner_pd;
	struct usb_power_delivery_capabilities *partner_src_caps;
	unsigned int partner_pd_gen;

	/* VDMs (vdm_discovery / vdm_debugfs) */
	bool altmode_sent;		/* 0x55 since probe / MCU reboot */
	struct usb_pd_identity partner_ident;
	u8 vdm_resp[3][VDM_RESP_LEN];	/* 0x4b sub 1..3, raw, this attach */
	bool vdm_resp_ok[3];
	struct mutex vdm_lock;		/* the debugfs request/reply below */
	u8 vdm_req[29];
	int vdm_req_len;
	u8 vdm_reply[VDM_ANSWER_LEN];
	int vdm_ret;
	unsigned long vdm_sent;		/* jiffies */

	struct power_supply *psy;
	struct mutex pub_lock;		/* pub, and pd's caps/strings for readers */
	struct usbc_pub pub;
	int notified_uv;		/* VBUS at the last power_supply_changed */

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
	u8 sysmsg = 0, pdmsg = 0;
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
	/* the PD event code now: one register, the next event overwrites it */
	if ((f[2] & PD_PDMSG) && usbc_read(u, REG_PD_STATUS0, &pdmsg, 1))
		f[2] &= ~PD_PDMSG;

	spin_lock_irqsave(&u->ev_lock, flags);
	if (f[2] & PD_PDMSG) {
		if (u->pdq_n < PDQ_N)
			u->pdq[u->pdq_n++] = pdmsg;
		else
			u->pdq_lost++;
	}
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
	if (f[2] & PD_PDMSG)
		usbc_log(u, LOG_PDMSG, pdmsg, NULL, 0, NULL, 0, 0,
			 pdmsg_name(pdmsg));
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
	int ret;

	/* Samsung's alt-mode value: Discover ID/SVIDs/Modes/Enter Mode open */
	if (vdm_discovery)
		m[3] = 0xf0;
	ret = usbc_write(u, REG_UIC_INT_M, m, 4);

	if (!ret)
		u->vdm_unmasked = m[3] != 0xff;
	return ret;
}

/* ---- PDOs ----------------------------------------------------------------- */

#define HV_MAX_MV		9000	/* stock DT max_input_voltage */
#define HV_MAX_MW		15000	/* stock DT pd_charging_charge_power */
#define PD_MAX_MA		3000	/* stock DT max_input_current */
#define HV_DCP_MA		1650	/* stock 9V_TA input current */

static bool pdo_fixed(u32 pdo)
{
	return (pdo >> 30) == 0;	/* PDO_TYPE_FIXED */
}

static unsigned int pdo_mv(u32 pdo)
{
	return ((pdo >> 10) & 0x3ff) * 50;
}

static unsigned int pdo_ma(u32 pdo)
{
	return (pdo & 0x3ff) * 10;
}

/* what we would draw from a fixed PDO: its current, at most 3 A and 15 W */
static unsigned int pdo_use_ma(u32 pdo)
{
	unsigned int mv = pdo_mv(pdo), ma = min(pdo_ma(pdo), PD_MAX_MA);

	if (mv && mv * ma > HV_MAX_MW * 1000)
		ma = HV_MAX_MW * 1000 / mv;
	return ma;
}

static bool usbc_pdo_allowed(struct max77705_usbc *u, u8 pos)
{
	u32 pdo;

	if (pos < 1 || pos > u->pd.cur.n)
		return false;
	pdo = u->pd.cur.pdo[pos - 1];
	return pdo_fixed(pdo) && pdo_mv(pdo) && pdo_mv(pdo) <= HV_MAX_MV;
}

static int pd_caps_fmt(char *buf, size_t len, const struct pd_caps *c)
{
	int n = 0, i;

	for (i = 0; i < c->n && n < len; i++) {
		u32 p = c->pdo[i];

		if (pdo_fixed(p))
			n += scnprintf(buf + n, len - n, "%s%d:%umV/%umA", i ? " " : "",
				       i + 1, pdo_mv(p), pdo_ma(p));
		else
			n += scnprintf(buf + n, len - n, "%s%d:type%u/%08x", i ? " " : "",
				       i + 1, p >> 30, p);
	}
	if (!c->n)
		n += scnprintf(buf + n, len - n, "(none)");
	return n;
}

/* ---- VDM switches ------------------------------------------------------------ */

#define VDM_HDR_STRUCTURED	BIT(15)
#define VDM_HDR_CMD(h)		((h) & 0x1f)
#define VDM_HDR_CMDTYPE(h)	(((h) >> 6) & 0x3)	/* 0 REQ 1 ACK 2 NAK 3 BUSY */
#define VDM_HDR_SVID(h)		((h) >> 16)

/* 0x48: [n | 1<<3, header LE32, n-1 VDOs]; what debugfs may send */
static bool usbc_vdm_allowed(const u8 *w, int wlen)
{
	unsigned int n;
	u32 h;

	if (vdm_debugfs <= 0 || wlen < 5 || wlen > 29 || (wlen - 1) % 4)
		return false;
	n = (wlen - 1) / 4;
	if (w[0] != (n | 1 << 3))
		return false;
	h = get_unaligned_le32(&w[1]);
	if (vdm_debugfs >= 2)
		return true;
	return (h & VDM_HDR_STRUCTURED) && VDM_HDR_CMDTYPE(h) == 0 &&
	       VDM_HDR_SVID(h) == 0xff00 && n == 1 &&
	       VDM_HDR_CMD(h) >= 1 && VDM_HDR_CMD(h) <= 3;
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
	case OP_FW_AUTOIBUS:
		/* only "USBC fw on, auto IBUS off": stock's AUTOIBUS_AT_OFF */
		return wlen == 1 && w[0] == AUTOIBUS_AT_OFF;
	case OP_AFC:
		/* 5 V or 9 V TX bytes only: never 12 V (high nibble 8) */
		return wlen == 1 && ((w[0] >> 4) == 0 || (w[0] >> 4) == 4);
	case OP_QC2:
		/* 5 V and 9 V; never QC2's 12 V or 20 V */
		return wlen == 1 && (w[0] == QC2_5V || w[0] == QC2_9V);
	case OP_BC_CTRL2_W:
		/* only stock's AFC reset (back to 5 V) */
		return wlen == 1 && w[0] == BC_CTRL2_AFC_RESET;
	case OP_CUR_SRCCAP:
	case OP_GET_SRCCAP:
		return wlen == 0;
	case OP_SRCCAP_REQUEST:
		/* a fixed PDO of at most 9 V in the caps now in force */
		return wlen == 1 && usbc_pdo_allowed(u, w[0]);
	case OP_SET_ALTMODE:
		/* stock's ALTERNATE_MODE_START, and only when asked for */
		return vdm_discovery && wlen == 1 && w[0] == ALTMODE_SRC_VDM;
	case OP_GET_VDM_RESP:
		/* reads of what the firmware received; Samsung's sub-ids */
		return wlen == 1 && (w[0] == 1 || w[0] == 2 || w[0] == 3 ||
				     w[0] == 4 || w[0] == 6 || w[0] == 0x10 ||
				     w[0] == 0x11);
	case OP_SET_VDM_REQ:
		return usbc_vdm_allowed(w, wlen);
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

/* ---- charger type --------------------------------------------------------- */

/* BC1.2 and the proprietary signatures, as max77705_resolve_chgtyp() and
 * muic_vps_table do */
static enum usbc_kind usbc_bc_kind(u8 bc)
{
	static const enum usbc_kind prop[8] = {
		K_NONE, K_SAMSUNG_2A, K_APPLE_05A, K_APPLE_1A, K_APPLE_2A,
		K_APPLE_12W, K_DCP_3A, K_NONE,
	};
	enum usbc_kind p = prop[FIELD_GET(BC_PRCHGTYP, bc)];

	if (!(bc & BC_VBUSDET))
		return K_NONE;
	switch (FIELD_GET(BC_CHGTYP, bc)) {
	case CHGTYP_SDP:
		if (bc & BC_DCDTMO)
			return K_DCD_TIMEOUT;
		/* a Samsung/Apple signature on an SDP/CDP is an "Unofficial TA" */
		return (p != K_NONE && p != K_DCP_3A) ? p : K_SDP;
	case CHGTYP_CDP:
		return (p != K_NONE && p != K_DCP_3A) ? p : K_CDP;
	case CHGTYP_DCP:
		return p != K_NONE ? p : K_DCP;
	default:
		return K_UNKNOWN;
	}
}

static bool kind_is_dcp(enum usbc_kind k)
{
	return k == K_DCP || (k >= K_SAMSUNG_2A && k <= K_DCP_3A) ||
	       k == K_AFC_9V || k == K_QC2_9V;
}

/* the source's Rp current while the phone is its sink, mA (0: none) */
static unsigned int usbc_rp_ma(struct max77705_usbc *u)
{
	u8 cc0 = u->st[ST(REG_CC_STATUS0)];

	if (FIELD_GET(CC0_CCSTAT, cc0) != CCSTAT_SINK)
		return 0;
	switch (FIELD_GET(CC0_CCISTAT, cc0)) {
	case CCI_DEFAULT: return 500;
	case CCI_1_5A: return 1500;
	case CCI_3_0A: return 3000;
	default: return 0;
	}
}

/* VBADC (USBC_STATUS1[7:4]) is a 1 V-wide window: report its middle, uV */
static int usbc_vbus_uv(struct max77705_usbc *u)
{
	unsigned int code = FIELD_GET(STATUS1_VBADC, READ_ONCE(u->st[0]));

	if (!(READ_ONCE(u->st[ST(REG_BC_STATUS)]) & BC_VBUSDET) || code == 0)
		return 0;
	if (code == 1)
		return 4150000;			/* 3.8..4.5 V */
	return (code + 3) * 1000000;		/* 2: 4.5..5.5 V, ... */
}

static void usbc_compute(struct max77705_usbc *u, struct usbc_pub *n)
{
	u8 bc = u->st[ST(REG_BC_STATUS)];
	u8 ccstat = FIELD_GET(CC0_CCSTAT, u->st[ST(REG_CC_STATUS0)]);
	enum usbc_kind k = usbc_bc_kind(bc);
	unsigned int ma = kind_ma[k], rp = usbc_rp_ma(u), mv = 5000;

	memset(n, 0, sizeof(*n));
	n->online = (bc & BC_VBUSDET) && ccstat != CCSTAT_SOURCE;
	if (!n->online) {
		n->kind = K_NONE;
		n->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		return;
	}

	/* Type-C current is what the source guarantees whatever BC1.2 says */
	if (rp > ma) {
		ma = rp;
		if (k == K_SDP || k == K_DCD_TIMEOUT || k == K_UNKNOWN)
			k = rp >= 3000 ? K_RP_3A : K_RP_1_5A;
	}

	/* a PD contract, or AFC/QC2 at 9 V, is what is in force */
	if (u->pd.pos && u->pd.pos <= u->pd.cur.n && ccstat == CCSTAT_SINK &&
	    pdo_fixed(u->pd.cur.pdo[u->pd.pos - 1])) {
		u32 pdo = u->pd.cur.pdo[u->pd.pos - 1];

		k = K_PD;
		mv = pdo_mv(pdo);
		ma = pdo_use_ma(pdo);
	} else if (u->hv_mode == HV_AFC_9V || u->hv_mode == HV_QC2_9V) {
		k = u->hv_mode == HV_AFC_9V ? K_AFC_9V : K_QC2_9V;
		mv = 9000;
		ma = HV_DCP_MA;
	}
	/* while switching: the lower of the two sides */
	if (u->hv_prepare_ma)
		ma = min(ma, u->hv_prepare_ma);

	n->kind = k;
	n->cur_ua = ma * 1000;
	n->vmax_uv = mv * 1000;
	switch (k) {
	case K_SDP:
		n->usb_type = POWER_SUPPLY_USB_TYPE_SDP;
		break;
	case K_CDP:
		n->usb_type = POWER_SUPPLY_USB_TYPE_CDP;
		break;
	case K_PD:
		n->usb_type = POWER_SUPPLY_USB_TYPE_PD;
		break;
	default:
		if (kind_is_dcp(k))
			n->usb_type = POWER_SUPPLY_USB_TYPE_DCP;
		else if (ccstat == CCSTAT_SINK)
			n->usb_type = POWER_SUPPLY_USB_TYPE_C;
		else
			n->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		break;
	}
}

/* publish; a uevent only when something other than voltage_now changed */
static void usbc_publish(struct max77705_usbc *u)
{
	struct usbc_pub n;
	bool changed, vchanged;
	int uv;

	usbc_compute(u, &n);
	uv = n.online ? usbc_vbus_uv(u) : 0;
	mutex_lock(&u->pub_lock);
	changed = n.online != u->pub.online || n.kind != u->pub.kind ||
		  n.usb_type != u->pub.usb_type || n.cur_ua != u->pub.cur_ua ||
		  n.vmax_uv != u->pub.vmax_uv;
	u->pub = n;
	mutex_unlock(&u->pub_lock);
	/* the charger re-ramps its input current on a 1 V step of VBUS:
	 * tell it, but not about the VBADC window's own jitter */
	vchanged = abs(uv - u->notified_uv) >= 1000000;
	if (!changed && !vchanged)
		return;
	u->notified_uv = uv;
	if (!changed) {
		dev_dbg(u->dev, "VBUS now about %d mV\n", uv / 1000);
		if (u->psy)
			power_supply_changed(u->psy);
		return;
	}
	dev_info(u->dev, "%s: %s, %u mA, %u mV max (bc=%02x cc0=%02x pd1=%02x)\n",
		 n.online ? "online" : "offline", kind_name[n.kind],
		 n.cur_ua / 1000, n.vmax_uv / 1000, u->st[ST(REG_BC_STATUS)],
		 u->st[ST(REG_CC_STATUS0)], u->st[ST(REG_PD_STATUS1)]);
	if (u->psy)
		power_supply_changed(u->psy);
}

/* ---- power_supply --------------------------------------------------------- */

static const enum power_supply_property usbc_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
};

static int usbc_get_property(struct power_supply *psy,
			     enum power_supply_property psp,
			     union power_supply_propval *val)
{
	struct max77705_usbc *u = power_supply_get_drvdata(psy);
	struct usbc_pub p;

	mutex_lock(&u->pub_lock);
	p = u->pub;
	mutex_unlock(&u->pub_lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = p.online;
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = p.usb_type;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = p.cur_ua;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = p.online ? usbc_vbus_uv(u) : 0;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = p.vmax_uv;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static ssize_t charger_type_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct max77705_usbc *u = power_supply_get_drvdata(to_power_supply(dev));
	enum usbc_kind k;

	mutex_lock(&u->pub_lock);
	k = u->pub.kind;
	mutex_unlock(&u->pub_lock);
	return sysfs_emit(buf, "%s\n", kind_name[k]);
}
static DEVICE_ATTR_RO(charger_type);

/*
 * hv_enable: 1 lets the driver ask for 9 V (AFC, QC2 or a fixed PDO, at
 * most 15 W); 0 (the default) keeps or brings the bus back to 5 V. Written
 * by zrn-charge, which drops the input limit first when it writes 0.
 */
static ssize_t hv_enable_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct max77705_usbc *u = power_supply_get_drvdata(to_power_supply(dev));

	return sysfs_emit(buf, "%d\n", READ_ONCE(u->hv_enable));
}

static ssize_t hv_enable_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t len)
{
	struct max77705_usbc *u = power_supply_get_drvdata(to_power_supply(dev));
	bool v;
	int ret = kstrtobool(buf, &v);

	if (ret)
		return ret;
	if (READ_ONCE(u->stopping))
		return -ENODEV;
	if (v && !READ_ONCE(u->hv_enable))
		WRITE_ONCE(u->hv_rearm, true);
	WRITE_ONCE(u->hv_enable, v);
	mod_delayed_work(u->wq, &u->hv_work, 0);
	return len;
}
static DEVICE_ATTR_RW(hv_enable);

#define PD_COUNTER_ATTR(name, field)					\
static ssize_t name##_show(struct device *dev,				\
			   struct device_attribute *attr, char *buf)	\
{									\
	struct max77705_usbc *u = power_supply_get_drvdata(to_power_supply(dev)); \
									\
	return sysfs_emit(buf, "%u\n", READ_ONCE(u->pd.field));		\
}									\
static DEVICE_ATTR_RO(name)

/* every hard reset; then the ones the source sent (our draw or our
 * messages) and the ones our firmware sent (our state machine) */
PD_COUNTER_ATTR(pd_hard_resets, hard_resets);
PD_COUNTER_ATTR(pd_hard_resets_received, hr_received);
PD_COUNTER_ATTR(pd_hard_resets_sent, hr_sent);
PD_COUNTER_ATTR(pd_hard_resets_no_pd, hr_no_pd);
/* new caps with no hard reset: a normal renegotiation (thermal, multi-port) */
PD_COUNTER_ATTR(pd_caps_renegotiated, caps_renegotiated);
/* caps after a hard reset differ from before it: the anomaly (in practice
 * fewer or smaller PDOs; any difference counts) */
PD_COUNTER_ATTR(pd_caps_reduced_after_hard_reset, caps_changed_after_hr);

/* the source caps in force, "mV:mA" per PDO in order (other PDO types as
 * typeN:raw); empty without a PD contract on this attach */
static ssize_t pd_source_caps_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct max77705_usbc *u = power_supply_get_drvdata(to_power_supply(dev));
	ssize_t n = 0;
	unsigned int i;

	mutex_lock(&u->pub_lock);
	for (i = 0; i < u->pd.cur.n; i++) {
		u32 p = u->pd.cur.pdo[i];

		if (pdo_fixed(p))
			n += sysfs_emit_at(buf, n, "%s%u:%u", i ? " " : "",
					   pdo_mv(p), pdo_ma(p));
		else
			n += sysfs_emit_at(buf, n, "%stype%u:%08x", i ? " " : "",
					   p >> 30, p);
	}
	mutex_unlock(&u->pub_lock);
	n += sysfs_emit_at(buf, n, "\n");
	return n;
}
static DEVICE_ATTR_RO(pd_source_caps);

static ssize_t pd_string_show(struct max77705_usbc *u, const char *s, char *buf)
{
	ssize_t n;

	mutex_lock(&u->pub_lock);
	n = sysfs_emit(buf, "%s\n", s);
	mutex_unlock(&u->pub_lock);
	return n;
}

static ssize_t pd_last_event_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct max77705_usbc *u = power_supply_get_drvdata(to_power_supply(dev));

	return pd_string_show(u, u->pd.last_event, buf);
}
static DEVICE_ATTR_RO(pd_last_event);

static ssize_t pd_last_hard_reset_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct max77705_usbc *u = power_supply_get_drvdata(to_power_supply(dev));

	return pd_string_show(u, u->pd.last_hard_reset, buf);
}
static DEVICE_ATTR_RO(pd_last_hard_reset);

static struct attribute *usbc_psy_attrs[] = {
	&dev_attr_charger_type.attr,
	&dev_attr_hv_enable.attr,
	&dev_attr_pd_hard_resets.attr,
	&dev_attr_pd_hard_resets_received.attr,
	&dev_attr_pd_hard_resets_sent.attr,
	&dev_attr_pd_hard_resets_no_pd.attr,
	&dev_attr_pd_caps_renegotiated.attr,
	&dev_attr_pd_caps_reduced_after_hard_reset.attr,
	&dev_attr_pd_last_event.attr,
	&dev_attr_pd_last_hard_reset.attr,
	&dev_attr_pd_source_caps.attr,
	NULL
};
ATTRIBUTE_GROUPS(usbc_psy);

static char *usbc_supplied_to[] = { "max77705-charger" };

static const struct power_supply_desc usbc_psy_desc = {
	.name		= "max77705-usbc",
	.type		= POWER_SUPPLY_TYPE_USB,
	.usb_types	= BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
			  BIT(POWER_SUPPLY_USB_TYPE_SDP) |
			  BIT(POWER_SUPPLY_USB_TYPE_DCP) |
			  BIT(POWER_SUPPLY_USB_TYPE_CDP) |
			  BIT(POWER_SUPPLY_USB_TYPE_C) |
			  BIT(POWER_SUPPLY_USB_TYPE_PD),
	.properties	= usbc_props,
	.num_properties	= ARRAY_SIZE(usbc_props),
	.get_property	= usbc_get_property,
};

/* ---- D+/D- routing -------------------------------------------------------- */

/*
 * As stock's muic_vps_table: the switch goes to the AP (COM_USB) only for a
 * host port (SDP, CDP) and a DCD timeout, and is open (COM_OPEN) for every
 * charger (TA, unofficial TA, AFC, QC). An open switch is what AFC and QC2
 * need: the MCU drives D+/D- itself, and the DWC3 PHY's pull-up on a DCP's
 * shorted lines is not something a charger expects.
 *
 * Kept from the old driver: with nothing to decide on (no VBUS, or BC1.2 not
 * finished) at start or after an MCU reboot the switch goes to the AP, and a
 * resolved attach writes CONTROL1 again even when the value is the same,
 * since the MCU may have dropped it. Unlike stock, a detach changes nothing.
 * One write per attach and value: a failure is not retried in a loop.
 */
static u8 usbc_path_for(enum usbc_kind k)
{
	switch (k) {
	case K_SDP:
	case K_CDP:
	case K_DCD_TIMEOUT:
		return COM_USB;
	default:
		return kind_is_dcp(k) ? COM_OPEN : 0;
	}
}

static void usbc_route(struct max77705_usbc *u)
{
	u8 want = usbc_path_for(usbc_bc_kind(u->st[ST(REG_BC_STATUS)]));

	if (!want) {
		if (u->path)
			return;
		want = COM_USB;
	}
	if (want == u->path_tried)
		return;
	u->path_tried = want;
	usbc_set_path(u, want);
}

/* ---- AutoIBUS --------------------------------------------------------------- */

static int usbc_autoibus_send(struct max77705_usbc *u, const char *why)
{
	u8 v = AUTOIBUS_AT_OFF;
	int ret = usbc_cmd(u, OP_FW_AUTOIBUS, &v, 1, NULL, 0);

	if (!ret)
		dev_info(u->dev, "AutoIBUS off (%s)\n", why);
	else if (ret != -EPERM && ret != -ENODEV)
		dev_warn(u->dev, "AutoIBUS off (%s) failed: %d\n", why, ret);
	return ret;
}

/*
 * DETAILS_00 bit 7 through the charger's own regmap (no cache, plain read;
 * the charger is found by its power_supply name, so no DT link is needed).
 * Returns 1 if the USBC firmware set the limit, 0 if the charger did, or a
 * negative error.
 */
static int usbc_ilim_by_usbc(void)
{
	struct power_supply *chg = power_supply_get_by_name("max77705-charger");
	struct regmap *map;
	unsigned int v;
	int ret = -ENODEV;

	if (!chg)
		return ret;
	map = chg->dev.parent ? dev_get_regmap(chg->dev.parent, NULL) : NULL;
	if (map) {
		ret = regmap_read(map, MAX77705_CHG_REG_DETAILS_00, &v);
		if (!ret)
			ret = !(v & CHG_DETAILS_00_ILIM_BY_CHG);
	}
	power_supply_put(chg);
	return ret;
}

static void usbc_autoibus(struct max77705_usbc *u, bool vbus)
{
	if (!autoibus_off || !u->mbox_allowed || u->mbox_dead)
		return;
	if (!u->autoibus_sent) {
		/* stock: max77705_pd_init(), once */
		u->autoibus_sent = true;
		usbc_autoibus_send(u, "start");
		return;
	}
	if (!vbus) {
		u->autoibus_resends = 0;
		return;
	}
	if (u->autoibus_resends >= AUTOIBUS_MAX_PER_ATTACH ||
	    time_before(jiffies, u->autoibus_checked +
			msecs_to_jiffies(AUTOIBUS_CHECK_MS)))
		return;
	u->autoibus_checked = jiffies;
	if (usbc_ilim_by_usbc() != 1)
		return;
	if (++u->autoibus_resends == AUTOIBUS_MAX_PER_ATTACH)
		dev_warn(u->dev, "AutoIBUS keeps coming back: last resend for this attach\n");
	usbc_autoibus_send(u, "DETAILS_00 says the USBC set ILIM");
}

/* ---- high voltage: shared ------------------------------------------------- */

#define HV_AFC_DELAY_MS		500	/* stock: "afc work after 500ms" */
#define HV_WAIT_MS		1000	/* recheck of the charger's limit */
#define HV_WAIT_MAX		10	/* then give up for this attach */
#define HV_RETRIES		3	/* stock RETRY_COUNT */
#define PD_REQ_TRIES		2
#define PD_REQ_WAIT_MS		2000	/* for PSRDY after a request */

/* the charger's input limit, uA, or a negative error */
static int usbc_chg_ilim_ua(void)
{
	struct power_supply *chg = power_supply_get_by_name("max77705-charger");
	union power_supply_propval v;
	int ret;

	if (!chg)
		return -ENODEV;
	ret = power_supply_get_property(chg, POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, &v);
	power_supply_put(chg);
	return ret ? ret : v.intval;
}

/*
 * Before the bus voltage goes up, the input limit has to be at or below what
 * the source will give at the new voltage: drawing 5 V-sized current at 9 V
 * is exactly what makes a charger hard-reset. The kernel does not set the
 * limit (zrn-charge does, from current_max); this only refuses to go up
 * until it has come down. The charger reads (code+1)*25 mA: 25 mA slack.
 */
static bool usbc_limit_ok(struct max77705_usbc *u, unsigned int target_ma)
{
	int ua = usbc_chg_ilim_ua();

	if (ua < 0) {
		dev_warn_once(u->dev, "no input limit to read (%d): staying at 5 V\n", ua);
		return false;
	}
	return ua <= (int)(target_ma + 25) * 1000;
}

/* ---- USB-PD (sink) ---------------------------------------------------------- */

static bool pd_caps_equal(const struct pd_caps *a, const struct pd_caps *b)
{
	return a->n == b->n && !memcmp(a->pdo, b->pdo, a->n * sizeof(a->pdo[0]));
}

/* stock's make_pd_list(), in effect: fixed PDOs up to 9 V, each worth
 * min(I, 3 A, 15 W / V); the most power, the lower voltage on a tie */
static u8 usbc_pd_select(struct max77705_usbc *u)
{
	unsigned int best_mw = 0, best_mv = 0, i;
	u8 best = 0;

	for (i = 0; i < u->pd.cur.n; i++) {
		u32 p = u->pd.cur.pdo[i];
		unsigned int mv = pdo_mv(p), mw;

		if (!usbc_pdo_allowed(u, i + 1))
			continue;
		mw = mv * pdo_use_ma(p) / 1000;
		if (mw > best_mw || (mw == best_mw && mv < best_mv)) {
			best = i + 1;
			best_mw = mw;
			best_mv = mv;
		}
	}
	return best;
}

static void usbc_pd_forget(struct max77705_usbc *u)
{
	mutex_lock(&u->pub_lock);
	u->pd.have_first = false;
	memset(&u->pd.first, 0, sizeof(u->pd.first));
	memset(&u->pd.cur, 0, sizeof(u->pd.cur));
	u->pd_gen++;
	u->pd.pos = 0;
	u->pd.req_pos = 0;
	u->pd.req_tries = 0;
	u->pd.hv_blocked = false;
	u->pd.hr_pending = false;
	mutex_unlock(&u->pub_lock);
}

/* 0x30: the source caps and the position of the contract */
static void usbc_pd_read_caps(struct max77705_usbc *u)
{
	u8 r[SRCCAP_ANSWER_LEN];
	char a[160], b[160];
	struct pd_caps c = {};
	bool hr, changed = false;
	unsigned int i;
	u8 pos;

	if (usbc_cmd(u, OP_CUR_SRCCAP, NULL, 0, r, sizeof(r)))
		return;
	pos = (r[0] >> 3) & 0x7;
	c.n = min_t(unsigned int, r[0] & 0x7, PD_MAX_PDO);
	for (i = 0; i < c.n; i++)
		c.pdo[i] = get_unaligned_le32(&r[1 + 4 * i]);
	if (pos > c.n)
		pos = 0;

	mutex_lock(&u->pub_lock);
	hr = u->pd.hr_pending;
	u->pd.hr_pending = false;
	if (!u->pd.have_first) {
		u->pd.have_first = true;
		u->pd.first = c;
	} else if (!pd_caps_equal(&u->pd.cur, &c)) {
		changed = true;
		u->pd.before = u->pd.cur;
		u->pd.after = c;
	}
	if (hr && !pd_caps_equal(&u->pd.hr_caps, &c)) {
		/* the anomaly: we were hard-reset and the source offers less */
		u->pd.caps_changed_after_hr++;
		u->pd.before = u->pd.hr_caps;
		u->pd.after = c;
		changed = true;
	} else if (changed && !hr) {
		u->pd.caps_renegotiated++;
	}
	if (!pd_caps_equal(&u->pd.cur, &c))
		u->pd_gen++;
	u->pd.cur = c;
	u->pd.pos = pos;
	if (pos && pos == u->pd.req_pos) {
		u->pd.req_pos = 0;
		u->pd.req_tries = 0;
	}
	if (changed)
		scnprintf(u->pd.last_event, sizeof(u->pd.last_event),
			  "%lld s: caps %s",
			  ktime_to_ms(ktime_get_boottime()) / 1000,
			  hr ? "changed after a hard reset" : "renegotiated");
	mutex_unlock(&u->pub_lock);

	pd_caps_fmt(b, sizeof(b), &c);
	if (changed) {
		pd_caps_fmt(a, sizeof(a), &u->pd.before);
		if (hr)
			dev_warn(u->dev, "PD: source caps after a hard reset differ: before [%s] after [%s]; only the new ones are used\n",
				 a, b);
		else
			dev_info(u->dev, "PD: source renegotiated its caps: before [%s] after [%s]\n",
				 a, b);
		usbc_log(u, LOG_NOTE, 0, NULL, 0, NULL, 0, 0,
			 hr ? "caps changed after hard reset" : "caps renegotiated");
		/* a uevent even if nothing published changes */
		power_supply_changed(u->psy);
	} else {
		dev_info(u->dev, "PD: contract pdo %u of [%s]\n", pos, b);
	}
	if (!u->pd.req_pos) {		/* nothing of ours still pending */
		u->hv_prepare_ma = 0;
		u->hv_waits = 0;
	}
}

static void usbc_pd_hard_reset(struct max77705_usbc *u, u8 code)
{
	bool theirs = code == 0x16;	/* HARDRESET_RECEIVED; 0x19 _SENT is ours */
	u32 pdo = u->pd.pos ? u->pd.cur.pdo[u->pd.pos - 1] : 0;
	int ilim = usbc_chg_ilim_ua();
	long since = u->pd.req_jiffies ?
		(long)jiffies_to_msecs(jiffies - u->pd.req_jiffies) : -1;

	/*
	 * Ours, with no PD seen on this attach at all: the firmware's own "no
	 * Source_Capabilities came" reset (SinkWaitCap, then Hard Reset, as the
	 * spec has a sink do), on every plain DCP/AFC/QC charger. Not a sign of
	 * anything wrong: counted apart, and nothing is blocked or remembered.
	 */
	if (!theirs && !u->pd.pos && !u->pd.req_pos && !u->pd.have_first) {
		mutex_lock(&u->pub_lock);
		u->pd.hr_no_pd++;
		mutex_unlock(&u->pub_lock);
		dev_info(u->dev, "PD: no source caps from this charger: our firmware's hard reset (not PD)\n");
		return;
	}

	mutex_lock(&u->pub_lock);
	u->pd.hard_resets++;
	if (theirs)
		u->pd.hr_received++;
	else
		u->pd.hr_sent++;
	scnprintf(u->pd.last_hard_reset, sizeof(u->pd.last_hard_reset),
		  "%lld s: hard reset %s; contract pdo %u (%u mV %u mA), requested pdo %u, hv_enable %d, input limit %d mA, vbus %d mV, %ld ms after our last request",
		  ktime_to_ms(ktime_get_boottime()) / 1000,
		  theirs ? "from the source" : "sent by our firmware",
		  u->pd.pos, pdo ? pdo_mv(pdo) : 0, pdo ? pdo_ma(pdo) : 0,
		  u->pd.req_pos, u->hv_enable, ilim < 0 ? ilim : ilim / 1000,
		  usbc_vbus_uv(u) / 1000, since);
	strscpy(u->pd.last_event, u->pd.last_hard_reset, sizeof(u->pd.last_event));
	if (!u->pd.hr_pending) {
		u->pd.hr_pending = true;
		u->pd.hr_caps = u->pd.cur;
	}
	/* went above 5 V and got reset: no more of that on this attach */
	if ((pdo && pdo_mv(pdo) > 5000) || u->pd.req_pos > 1)
		u->pd.hv_blocked = true;
	u->pd.pos = 0;
	u->pd.req_pos = 0;
	u->pd.req_tries = 0;
	mutex_unlock(&u->pub_lock);

	u->hv_prepare_ma = 0;
	dev_warn(u->dev, "PD: %s%s\n", u->pd.last_hard_reset,
		 u->pd.hv_blocked ? "; above 5 V blocked until detach or hv_enable 0->1" : "");
	power_supply_changed(u->psy);
}

/* one PD event code (PD_STATUS0), in the order they came */
static void usbc_pd_event(struct max77705_usbc *u, u8 code)
{
	switch (code) {
	case 0x16:	/* HARDRESET_RECEIVED */
	case 0x19:	/* HARDRESET_SENT */
		usbc_pd_hard_reset(u, code);
		break;
	case 0x02:	/* SNK/SRC error recovery */
	case 0x05:
		u->pd.err_recovery++;
		dev_warn(u->dev, "PD: %s\n", pdmsg_name(code));
		break;
	case 0x03:	/* SenderResponseTimer */
	case 0x06:
		u->pd.srt_timeouts++;
		dev_info(u->dev, "PD: %s\n", pdmsg_name(code));
		break;
	default:
		break;
	}
}

/* ---- AFC / QC2 ------------------------------------------------------------ */

static bool vbadc_is_9v(u8 v)
{
	return v >= 5 && v <= 7;	/* 7.5..10.5 V */
}

static u8 usbc_vbadc(struct max77705_usbc *u)
{
	usbc_read_status(u);
	return FIELD_GET(STATUS1_VBADC, u->st[0]);
}

/* still a DCP, switch open, not PD, VBUS there */
static bool usbc_dcp_attached(struct max77705_usbc *u)
{
	u8 bc = u->st[ST(REG_BC_STATUS)];

	return (bc & BC_VBUSDET) && FIELD_GET(BC_CHGTYP, bc) == CHGTYP_DCP &&
	       u->path == COM_OPEN && !(u->st[ST(REG_PD_STATUS1)] & PD1_PSRDY) &&
	       !u->pd.have_first;
}

static void usbc_afc_reset(struct max77705_usbc *u)
{
	u8 v = BC_CTRL2_AFC_RESET;

	dev_info(u->dev, "AFC/QC2: reset to 5 V\n");
	usbc_cmd(u, OP_BC_CTRL2_W, &v, 1, NULL, 0);
}

/*
 * QC2 at 9 V (stock: only after AFC's master ping is NACKed): 0x22/0x09,
 * answer [result, VBADC], three retries. Returns true at 9 V.
 */
static bool usbc_qc2_9v(struct max77705_usbc *u)
{
	u8 v = QC2_9V, r[QC2_ANSWER_LEN];
	unsigned int tries = 0;

	while (tries++ <= HV_RETRIES) {
		if (!usbc_dcp_attached(u) || !u->hv_enable)
			return false;
		u->pd.req_jiffies = jiffies;
		if (usbc_cmd(u, OP_QC2, &v, 1, r, sizeof(r)))
			return false;
		if (!r[1])
			r[1] = usbc_vbadc(u);
		dev_info(u->dev, "QC2: result %u, vbadc %u\n", r[0], r[1]);
		switch (r[0]) {
		case 0:
			return vbadc_is_9v(r[1]);
		case 1: case 2: case 3:		/* no CHGIN, not HVDCP, not DCP */
			return false;
		default:			/* 6, 7, ...: retry */
			if (tries > HV_RETRIES && r[1] >= 5)
				usbc_afc_reset(u);
			break;
		}
	}
	return false;
}

/*
 * AFC 9 V as max77705_muic_afc_hv_set()/handle_detect_dev_afc(): 0x21 with
 * TX byte 0x46, answer [result, VBADC, RX bytes]. Result 0 with VBADC in
 * the 9 V window is success; 4 (master ping NACK) goes to QC2; 5 picks a
 * 9 V byte from the charger's own list; NACK-type results retry three
 * times, then reset to 5 V if VBUS went up anyway.
 */
static enum hv_mode usbc_afc_9v(struct max77705_usbc *u)
{
	u8 tx = AFC_TX_9V, r[AFC_ANSWER_LEN];
	unsigned int tries = 0, i;

	while (tries++ <= HV_RETRIES) {
		if (!usbc_dcp_attached(u) || !u->hv_enable)
			return HV_NONE;
		u->pd.req_jiffies = jiffies;
		if (usbc_cmd(u, OP_AFC, &tx, 1, r, sizeof(r)))
			return HV_NONE;
		if (!r[1])
			r[1] = usbc_vbadc(u);	/* stock's W/A */
		dev_info(u->dev, "AFC: tx %02x result %u vbadc %u rx %*phN\n",
			 tx, r[0], r[1], 8, &r[2]);
		switch (r[0]) {
		case 0:
			return vbadc_is_9v(r[1]) ? HV_AFC_9V : HV_NONE;
		case 4:				/* MPing NACK: not AFC, try QC2 */
			return usbc_qc2_9v(u) ? HV_QC2_9V : HV_NONE;
		case 5:				/* unsupported TX: take its 9 V byte */
			for (i = 2; i < AFC_ANSWER_LEN && r[i]; i++)
				if ((r[i] & 0xf0) == 0x40)
					break;
			if (i == AFC_ANSWER_LEN || !r[i])
				return HV_NONE;
			tx = r[i];
			break;
		case 1: case 2: case 3: case 11: /* not an HV DCP after all */
		case 8: case 9: case 10:	/* CC/SBU shorts */
			return HV_NONE;
		default:			/* 6, 7, 16..18: NACK, retry */
			if (tries > HV_RETRIES && r[1] >= 5)
				usbc_afc_reset(u);
			break;
		}
	}
	return HV_NONE;
}

/* back to 5 V on hv_enable 0: AFC 0x08 / QC2 0x04, else the reset */
static void usbc_dcp_5v(struct max77705_usbc *u)
{
	u8 r[AFC_ANSWER_LEN] = {}, v;
	int ret;

	if (u->hv_mode == HV_AFC_9V) {
		v = AFC_TX_5V;
		ret = usbc_cmd(u, OP_AFC, &v, 1, r, AFC_ANSWER_LEN);
	} else {
		v = QC2_5V;
		ret = usbc_cmd(u, OP_QC2, &v, 1, r, QC2_ANSWER_LEN);
	}
	if (!r[1])
		r[1] = usbc_vbadc(u);
	if (ret || r[0] || vbadc_is_9v(r[1])) {
		dev_warn(u->dev, "back to 5 V: ret %d result %u vbadc %u: resetting\n",
			 ret, r[0], r[1]);
		usbc_afc_reset(u);
	}
	dev_info(u->dev, "%s: back to 5 V\n", u->hv_mode == HV_AFC_9V ? "AFC" : "QC2");
	u->hv_mode = HV_NONE;
}

/* ---- high voltage: the policy step ------------------------------------------- */

/* returns ms until it wants to run again, or 0 */
static unsigned int usbc_hv_step(struct max77705_usbc *u)
{
	u8 ccstat = FIELD_GET(CC0_CCSTAT, u->st[ST(REG_CC_STATUS0)]);

	if (!u->mbox_allowed || u->mbox_dead || !u->pub.online)
		return 0;

	/* USB-PD: we only pick among fixed PDOs the firmware has */
	if (ccstat == CCSTAT_SINK && u->pd.pos) {
		u32 cur = u->pd.cur.pdo[u->pd.pos - 1];
		u8 want = 1, pos;

		if (u->hv_enable && !u->pd.hv_blocked)
			want = usbc_pd_select(u);
		if (!want || want == u->pd.pos)
			return 0;
		if (u->pd.req_pos == want) {
			unsigned long due = u->pd.req_jiffies +
					    msecs_to_jiffies(PD_REQ_WAIT_MS);

			/* asked already: PSRDY and the 0x30 read end it */
			if (time_before(jiffies, due))
				return jiffies_to_msecs(due - jiffies) + 1;
			if (u->pd.req_tries >= PD_REQ_TRIES) {
				if (u->hv_prepare_ma)
					dev_warn(u->dev, "PD: pdo %u not granted, staying at pdo %u\n",
						 want, u->pd.pos);
				u->hv_prepare_ma = 0;
				return 0;
			}
		}
		if (pdo_mv(u->pd.cur.pdo[want - 1]) > pdo_mv(cur)) {
			unsigned int ma = pdo_use_ma(u->pd.cur.pdo[want - 1]);

			if (!usbc_limit_ok(u, ma)) {
				u->hv_prepare_ma = ma;
				if (u->hv_waits++ < HV_WAIT_MAX)
					return HV_WAIT_MS;
				dev_warn(u->dev, "PD: input limit not at %u mA after %d s: staying at pdo %u\n",
					 ma, HV_WAIT_MAX * HV_WAIT_MS / 1000, u->pd.pos);
				u->pd.req_pos = want;
				u->pd.req_tries = PD_REQ_TRIES;
				u->hv_prepare_ma = 0;
				return 0;
			}
			u->hv_prepare_ma = ma;
		} else {
			u->hv_prepare_ma = 0;
		}
		u->hv_waits = 0;
		if (u->pd.req_pos != want)
			u->pd.req_tries = 0;
		u->pd.req_pos = want;
		u->pd.req_tries++;
		u->pd.req_jiffies = jiffies;
		pos = want;
		dev_info(u->dev, "PD: requesting pdo %u (%u mV %u mA)\n", pos,
			 pdo_mv(u->pd.cur.pdo[pos - 1]), pdo_ma(u->pd.cur.pdo[pos - 1]));
		if (usbc_cmd(u, OP_SRCCAP_REQUEST, &pos, 1, NULL, 0))
			u->hv_prepare_ma = 0;
		/* PSRDY then re-reads 0x30 and clears hv_prepare_ma */
		return 0;
	}

	/* AFC / QC2 on a DCP */
	if (!u->hv_enable) {
		u->hv_prepare_ma = 0;
		u->hv_waits = 0;
	}
	if (u->hv_mode != HV_NONE && !u->hv_enable) {
		usbc_dcp_5v(u);
		return 0;
	}
	if (u->hv_mode != HV_NONE || !u->hv_enable || u->hv_dcp_done ||
	    !usbc_dcp_attached(u))
		return 0;
	/* stock's PREPARE_TA: 500 mA while it switches; 1650 at most after */
	u->hv_prepare_ma = 500;
	if (!usbc_limit_ok(u, HV_DCP_MA)) {
		if (u->hv_waits++ < HV_WAIT_MAX)
			return HV_WAIT_MS;
		dev_warn(u->dev, "AFC: input limit not at %u mA after %d s: staying at 5 V\n",
			 HV_DCP_MA, HV_WAIT_MAX * HV_WAIT_MS / 1000);
		u->hv_dcp_done = true;
		u->hv_prepare_ma = 0;
		return 0;
	}
	u->hv_waits = 0;
	u->hv_dcp_done = true;
	u->hv_mode = usbc_afc_9v(u);
	u->hv_prepare_ma = 0;
	dev_info(u->dev, "AFC/QC2: %s\n", u->hv_mode == HV_AFC_9V ? "AFC 9 V" :
		 u->hv_mode == HV_QC2_9V ? "QC2 9 V" : "stays at 5 V");
	return 0;
}

static void usbc_hv_work(struct work_struct *work)
{
	struct max77705_usbc *u = container_of(to_delayed_work(work),
					       struct max77705_usbc, hv_work);
	unsigned int again;

	/* hv_enable went 0 -> 1: what this attach had given up on, try again */
	if (xchg(&u->hv_rearm, false)) {
		u->pd.hv_blocked = false;
		u->pd.req_tries = 0;
		u->hv_dcp_done = false;
		u->hv_waits = 0;
	}
	if (usbc_read_status(u))
		return;
	usbc_publish(u);	/* pub.online for the step */
	again = usbc_hv_step(u);
	usbc_publish(u);
	if (again)
		queue_delayed_work(u->wq, &u->hv_work, msecs_to_jiffies(again));
}

/* ---- Type-C class ----------------------------------------------------------- */

#if IS_ENABLED(CONFIG_TYPEC)
/*
 * What the firmware shows, mapped onto the typec class: a port (DRP/DRD,
 * no role-swap ops: the firmware decides), a partner per CC attach with
 * orientation, roles, Rp/PD power operation mode, and, once a contract is
 * in, a usb_power_delivery object with the partner's source capabilities
 * (/sys/class/usb_power_delivery/pdN/source-capabilities). The partner's
 * PD revision is not reported by the firmware: 3.0 is the port's (Samsung
 * hard-codes 0x300 too). No cable: the firmware gives nothing about SOP'.
 */
#define PD_REV_PORT	0x0300

static void usbc_typec_drop_pd(struct max77705_usbc *u)
{
	if (!u->partner_pd)
		return;
	typec_partner_set_usb_power_delivery(u->partner, NULL);
	if (!IS_ERR_OR_NULL(u->partner_src_caps))
		usb_power_delivery_unregister_capabilities(u->partner_src_caps);
	u->partner_src_caps = NULL;
	usb_power_delivery_unregister(u->partner_pd);
	u->partner_pd = NULL;
}

static void usbc_typec_detach(struct max77705_usbc *u)
{
	if (!u->partner)
		return;
	usbc_typec_drop_pd(u);
	typec_unregister_partner(u->partner);
	u->partner = NULL;
	typec_set_orientation(u->port, TYPEC_ORIENTATION_NONE);
	typec_set_pwr_opmode(u->port, TYPEC_PWR_MODE_USB);
	typec_set_pwr_role(u->port, TYPEC_SINK);
	typec_set_data_role(u->port, TYPEC_DEVICE);
	typec_set_vconn_role(u->port, TYPEC_SINK);
}

/* the source caps in force as the partner's usb_power_delivery */
static void usbc_typec_pd(struct max77705_usbc *u)
{
	struct usb_power_delivery_desc desc = { .revision = PD_REV_PORT };
	struct usb_power_delivery_capabilities_desc caps = { .role = TYPEC_SOURCE };

	if (u->partner_pd && u->partner_pd_gen == u->pd_gen)
		return;
	usbc_typec_drop_pd(u);
	u->partner_pd_gen = u->pd_gen;
	if (!u->pd.cur.n)
		return;

	typec_partner_set_pd_revision(u->partner, PD_REV_PORT);	/* usb_pd = 1 */
	u->partner_pd = typec_partner_usb_power_delivery_register(u->partner, &desc);
	if (IS_ERR(u->partner_pd)) {
		dev_warn(u->dev, "partner usb_power_delivery: %ld\n",
			 PTR_ERR(u->partner_pd));
		u->partner_pd = NULL;
		return;
	}
	memcpy(caps.pdo, u->pd.cur.pdo, u->pd.cur.n * sizeof(u32));
	u->partner_src_caps = usb_power_delivery_register_capabilities(u->partner_pd, &caps);
	if (IS_ERR(u->partner_src_caps))
		dev_warn(u->dev, "partner source caps: %ld\n", PTR_ERR(u->partner_src_caps));
	typec_partner_set_usb_power_delivery(u->partner, u->partner_pd);
}

static void usbc_typec_update(struct max77705_usbc *u)
{
	u8 cc0 = u->st[ST(REG_CC_STATUS0)], pd1 = u->st[ST(REG_PD_STATUS1)];
	u8 ccstat = FIELD_GET(CC0_CCSTAT, cc0);
	enum typec_pwr_opmode mode;

	if (!u->port)
		return;
	if (ccstat == CCSTAT_NONE || ccstat > 4) {	/* none, error, disabled */
		usbc_typec_detach(u);
		return;
	}
	if (!u->partner) {
		struct typec_partner_desc desc = {
			.identity = &u->partner_ident,
			.accessory = ccstat == 3 ? TYPEC_ACCESSORY_AUDIO :
				     ccstat == 4 ? TYPEC_ACCESSORY_DEBUG :
				     TYPEC_ACCESSORY_NONE,
		};

		u->partner = typec_register_partner(u->port, &desc);
		if (IS_ERR(u->partner)) {
			dev_warn(u->dev, "typec partner: %ld\n", PTR_ERR(u->partner));
			u->partner = NULL;
			return;
		}
		u->partner_pd_gen = u->pd_gen - 1;	/* look at the caps */
	}

	typec_set_pwr_role(u->port, ccstat == CCSTAT_SOURCE ? TYPEC_SOURCE : TYPEC_SINK);
	typec_set_data_role(u->port, (pd1 & PD1_DATAROLE) ? TYPEC_HOST : TYPEC_DEVICE);
	typec_set_vconn_role(u->port, (cc0 & CC0_CCVCNSTAT) ? TYPEC_SOURCE : TYPEC_SINK);
	switch (FIELD_GET(CC0_CCPINSTAT, cc0)) {
	case 1:
		typec_set_orientation(u->port, TYPEC_ORIENTATION_NORMAL);
		break;
	case 2:
		typec_set_orientation(u->port, TYPEC_ORIENTATION_REVERSE);
		break;
	default:
		typec_set_orientation(u->port, TYPEC_ORIENTATION_NONE);
		break;
	}

	if (u->pd.pos && ccstat == CCSTAT_SINK)
		mode = TYPEC_PWR_MODE_PD;
	else if (ccstat == CCSTAT_SINK && FIELD_GET(CC0_CCISTAT, cc0) == CCI_3_0A)
		mode = TYPEC_PWR_MODE_3_0A;
	else if (ccstat == CCSTAT_SINK && FIELD_GET(CC0_CCISTAT, cc0) == CCI_1_5A)
		mode = TYPEC_PWR_MODE_1_5A;
	else
		mode = TYPEC_PWR_MODE_USB;
	typec_set_pwr_opmode(u->port, mode);

	if (ccstat == CCSTAT_SINK)
		usbc_typec_pd(u);
}

static void usbc_typec_remove(void *data)
{
	struct max77705_usbc *u = data;

	usbc_typec_detach(u);
	typec_unregister_port(u->port);
	u->port = NULL;
}

static void usbc_typec_init(struct max77705_usbc *u)
{
	struct typec_capability cap = {
		.type = TYPEC_PORT_DRP,
		.data = TYPEC_PORT_DRD,
		.revision = USB_TYPEC_REV_1_2,
		.pd_revision = PD_REV_PORT,
		.prefer_role = TYPEC_NO_PREFERRED_ROLE,
		.accessory = { TYPEC_ACCESSORY_AUDIO, TYPEC_ACCESSORY_DEBUG },
		.orientation_aware = 1,
	};

	cap.fwnode = dev_fwnode(u->dev);
	u->port = typec_register_port(u->dev, &cap);
	if (IS_ERR(u->port)) {
		dev_warn(u->dev, "typec port: %ld (power_supply only)\n", PTR_ERR(u->port));
		u->port = NULL;
		return;
	}
	typec_set_pwr_role(u->port, TYPEC_SINK);
	typec_set_data_role(u->port, TYPEC_DEVICE);
	if (devm_add_action_or_reset(u->dev, usbc_typec_remove, u))
		u->port = NULL;
}
static void usbc_typec_identity(struct max77705_usbc *u)
{
	if (u->partner)
		typec_partner_set_identity(u->partner);
}
#else
static void usbc_typec_update(struct max77705_usbc *u) { }
static void usbc_typec_init(struct max77705_usbc *u) { }
static void usbc_typec_identity(struct max77705_usbc *u) { }
#endif

/* ---- VDM results (vdm_discovery) ------------------------------------------ */

static void usbc_vdm_forget(struct max77705_usbc *u)
{
	memset(&u->partner_ident, 0, sizeof(u->partner_ident));
	memset(u->vdm_resp_ok, 0, sizeof(u->vdm_resp_ok));
}

/*
 * 0x4b [sub] -> r: [sub, PD header LE16, VDM header LE32, VDOs LE32...].
 * Kept raw for debugfs; Discover Identity (an ACK) becomes the partner's
 * identity: ID header, cert stat, product, then up to three product-type
 * VDOs, as many as the PD header's object count says.
 */
static void usbc_vdm_read(struct max77705_usbc *u, u8 sub)
{
	u8 *r = u->vdm_resp[sub - 1];
	unsigned int nobj, i;
	u32 hdr;

	u->vdm_resp_ok[sub - 1] = false;
	if (usbc_cmd(u, OP_GET_VDM_RESP, &sub, 1, r, VDM_RESP_LEN))
		return;
	u->vdm_resp_ok[sub - 1] = true;
	hdr = get_unaligned_le32(&r[3]);
	nobj = (get_unaligned_le16(&r[1]) >> 12) & 0x7;	/* incl. the VDM header */
	dev_info(u->dev, "VDM %s: header %08x (%s), %u VDOs\n",
		 sub == VDM_SUB_IDENTITY ? "Discover Identity" :
		 sub == VDM_SUB_SVIDS ? "Discover SVIDs" : "Discover Modes",
		 hdr, VDM_HDR_CMDTYPE(hdr) == 1 ? "ACK" :
		 VDM_HDR_CMDTYPE(hdr) == 2 ? "NAK" : "BUSY/REQ",
		 nobj ? nobj - 1 : 0);
	if (sub != VDM_SUB_IDENTITY || VDM_HDR_CMDTYPE(hdr) != 1 || nobj < 4)
		return;

	memset(&u->partner_ident, 0, sizeof(u->partner_ident));
	u->partner_ident.id_header = get_unaligned_le32(&r[7]);
	u->partner_ident.cert_stat = get_unaligned_le32(&r[11]);
	u->partner_ident.product = get_unaligned_le32(&r[15]);
	for (i = 0; i < 3 && 4 + i < nobj && 19 + 4 * i + 4 <= VDM_RESP_LEN; i++)
		u->partner_ident.vdo[i] = get_unaligned_le32(&r[19 + 4 * i]);
	dev_info(u->dev, "partner identity: VID %04x PID %04x bcdDevice %04x (id header %08x)\n",
		 u->partner_ident.id_header & 0xffff, u->partner_ident.product >> 16,
		 u->partner_ident.product & 0xffff, u->partner_ident.id_header);
	usbc_typec_identity(u);
}

static void usbc_vdm_events(struct max77705_usbc *u, u8 vdm_int)
{
	if (!vdm_discovery)
		return;
	if (!u->altmode_sent && u->mbox_allowed && !u->mbox_dead) {
		u8 v = ALTMODE_SRC_VDM;

		u->altmode_sent = true;
		if (!usbc_cmd(u, OP_SET_ALTMODE, &v, 1, NULL, 0))
			dev_info(u->dev, "firmware discovery on (0x55 = 3)\n");
	}
	if (vdm_int & BIT(0))
		usbc_vdm_read(u, VDM_SUB_IDENTITY);
	if (vdm_int & BIT(1))
		usbc_vdm_read(u, VDM_SUB_SVIDS);
	if (vdm_int & BIT(2))
		usbc_vdm_read(u, VDM_SUB_MODES);
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
		u->path_tried = 0;
		u->autoibus_sent = false;
		u->altmode_sent = false;
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
	u8 pend[4], sysmsg, pdq[PDQ_N], ccstat;
	unsigned int pdq_n, lost, i;
	bool sysmsg_new, vbus, psrdy = false, dcp_was_open;
	unsigned long flags;

	spin_lock_irqsave(&u->ev_lock, flags);
	memcpy(pend, u->pend, sizeof(pend));
	memset(u->pend, 0, sizeof(u->pend));
	sysmsg = u->sysmsg;
	sysmsg_new = u->sysmsg_new;
	u->sysmsg_new = false;
	pdq_n = u->pdq_n;
	memcpy(pdq, u->pdq, pdq_n);
	u->pdq_n = 0;
	lost = u->pdq_lost;
	u->pdq_lost = 0;
	spin_unlock_irqrestore(&u->ev_lock, flags);

	if (sysmsg_new)
		usbc_handle_sysmsg(u, sysmsg);

	dcp_was_open = usbc_dcp_attached(u);
	if (usbc_read_status(u))
		return;
	vbus = u->st[ST(REG_BC_STATUS)] & BC_VBUSDET;
	ccstat = FIELD_GET(CC0_CCSTAT, u->st[ST(REG_CC_STATUS0)]);

	/* detach: PD per CC attach, AFC/QC2 per VBUS */
	if (ccstat != CCSTAT_SINK && (u->pd.have_first || u->pd.pos))
		usbc_pd_forget(u);
	if (ccstat == CCSTAT_NONE)
		usbc_vdm_forget(u);
	if (!vbus) {
		u->hv_mode = HV_NONE;
		u->hv_dcp_done = false;
		u->hv_waits = 0;
		if (!u->pd.req_pos)
			u->hv_prepare_ma = 0;
	}

	if (lost)
		dev_warn(u->dev, "PD: %u event codes lost\n", lost);
	for (i = 0; i < pdq_n; i++) {
		usbc_pd_event(u, pdq[i]);
		if (pdq[i] == 0x01)	/* SNK_PSRDY_RECEIVED */
			psrdy = true;
	}
	if (pend[2] & PD_PSRDY)
		psrdy = true;

	usbc_route(u);
	if (!vbus)
		u->path_tried = 0;	/* the next attach writes it again */
	usbc_autoibus(u, vbus);
	usbc_vdm_events(u, pend[3]);

	/* a contract as a sink (or one already there at probe): its caps */
	if (ccstat == CCSTAT_SINK && (u->st[ST(REG_PD_STATUS1)] & PD1_PSRDY) &&
	    (psrdy || !u->pd.have_first))
		usbc_pd_read_caps(u);
	usbc_typec_update(u);

	usbc_publish(u);

	if (!u->pub.online)
		return;
	if (u->pd.pos)
		queue_delayed_work(u->wq, &u->hv_work, 0);
	else if (usbc_dcp_attached(u) && !dcp_was_open)
		queue_delayed_work(u->wq, &u->hv_work,
				   msecs_to_jiffies(HV_AFC_DELAY_MS));
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

static int usbc_pd_show(struct seq_file *s, void *unused)
{
	struct max77705_usbc *u = s->private;
	struct usbc_pd *pd = &u->pd;
	char b[160];

	mutex_lock(&u->pub_lock);
	pd_caps_fmt(b, sizeof(b), &pd->first);
	seq_printf(s, "caps at attach: %s\n", b);
	pd_caps_fmt(b, sizeof(b), &pd->cur);
	seq_printf(s, "caps in force:  %s\n", b);
	seq_printf(s, "contract pdo:   %u\nrequested pdo:  %u (tries %u)\n",
		   pd->pos, pd->req_pos, pd->req_tries);
	seq_printf(s, "hv_enable %d, above 5 V blocked: %d, AFC/QC2: %s, ceiling while switching: %u mA\n",
		   u->hv_enable, pd->hv_blocked,
		   u->hv_mode == HV_AFC_9V ? "afc 9 V" :
		   u->hv_mode == HV_QC2_9V ? "qc2 9 V" : "none", u->hv_prepare_ma);
	seq_printf(s, "hard resets %u (from the source %u, sent by our firmware %u)\n",
		   pd->hard_resets, pd->hr_received, pd->hr_sent);
	seq_printf(s, "error recoveries %u, sender response timeouts %u\n",
		   pd->err_recovery, pd->srt_timeouts);
	seq_printf(s, "caps renegotiated %u, changed after a hard reset %u\n",
		   pd->caps_renegotiated, pd->caps_changed_after_hr);
	pd_caps_fmt(b, sizeof(b), &pd->before);
	seq_printf(s, "last change, before: %s\n", b);
	pd_caps_fmt(b, sizeof(b), &pd->after);
	seq_printf(s, "last change, after:  %s\n", b);
	seq_printf(s, "last event: %s\nlast hard reset: %s\n",
		   pd->last_event, pd->last_hard_reset);
	mutex_unlock(&u->pub_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(usbc_pd);

/*
 * vdm (vdm_debugfs >= 1): write "<VDM header> [VDO ...]" as hex words; the
 * driver frames 0x48 and the partner's reply comes back in the same
 * transaction. Read: the last request and reply, and the raw 0x4b results.
 * Only with a partner, a PD contract and no request of ours pending; one per
 * second; the allowlist (usbc_vdm_allowed) has the last word.
 */
static ssize_t usbc_vdm_write(struct max77705_usbc *u, const char __user *ubuf,
			      size_t len)
{
	char kbuf[128], *p, *tok;
	u8 w[29];
	int n = 0, ret;
	u32 v;

	if (len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, len))
		return -EFAULT;
	kbuf[len] = 0;
	p = strim(kbuf);
	while ((tok = strsep(&p, " \t\n")) && n < 7) {
		if (!*tok)
			continue;
		if (kstrtou32(tok, 16, &v))
			return -EINVAL;
		put_unaligned_le32(v, &w[1 + 4 * n]);
		n++;
	}
	if (!n || (p && *p))
		return -EINVAL;
	w[0] = n | 1 << 3;
	if (FIELD_GET(CC0_CCSTAT, READ_ONCE(u->st[ST(REG_CC_STATUS0)])) == CCSTAT_NONE ||
	    !READ_ONCE(u->pd.pos) || READ_ONCE(u->pd.req_pos))
		return -EAGAIN;

	mutex_lock(&u->vdm_lock);
	if (u->vdm_sent && time_before(jiffies, u->vdm_sent + HZ)) {
		mutex_unlock(&u->vdm_lock);
		return -EBUSY;
	}
	u->vdm_sent = jiffies;
	memcpy(u->vdm_req, w, 1 + 4 * n);
	u->vdm_req_len = 1 + 4 * n;
	memset(u->vdm_reply, 0, sizeof(u->vdm_reply));
	ret = usbc_cmd(u, OP_SET_VDM_REQ, w, 1 + 4 * n, u->vdm_reply, VDM_ANSWER_LEN);
	u->vdm_ret = ret;
	mutex_unlock(&u->vdm_lock);
	return ret ? ret : len;
}

static int usbc_vdm_show(struct seq_file *s, void *unused)
{
	struct max77705_usbc *u = s->private;
	static const char * const what[3] = { "identity", "svids", "modes" };
	int i;

	mutex_lock(&u->vdm_lock);
	seq_printf(s, "vdm_debugfs %d, vdm_discovery %d\n", vdm_debugfs, vdm_discovery);
	if (u->vdm_req_len) {
		seq_printf(s, "request: %*phN, ret %d\n", u->vdm_req_len, u->vdm_req,
			   u->vdm_ret);
		if (!u->vdm_ret && u->vdm_reply[0] == 0xff)
			seq_puts(s, "reply: none\n");
		else if (!u->vdm_ret)
			seq_printf(s, "reply: header %08x (%s), raw %*phN\n",
				   get_unaligned_le32(&u->vdm_reply[1]),
				   VDM_HDR_CMDTYPE(get_unaligned_le32(&u->vdm_reply[1])) == 1 ? "ACK" :
				   VDM_HDR_CMDTYPE(get_unaligned_le32(&u->vdm_reply[1])) == 2 ? "NAK" : "BUSY/REQ",
				   VDM_ANSWER_LEN, u->vdm_reply);
	}
	mutex_unlock(&u->vdm_lock);
	for (i = 0; i < 3; i++)
		if (u->vdm_resp_ok[i])
			seq_printf(s, "0x4b %s: %*phN\n", what[i], VDM_RESP_LEN, u->vdm_resp[i]);
	return 0;
}

static int usbc_vdm_open(struct inode *inode, struct file *file)
{
	return single_open(file, usbc_vdm_show, inode->i_private);
}

static ssize_t usbc_vdm_write_file(struct file *file, const char __user *ubuf,
				   size_t len, loff_t *ppos)
{
	struct seq_file *s = file->private_data;

	if (vdm_debugfs <= 0)
		return -EPERM;
	return usbc_vdm_write(s->private, ubuf, len);
}

static const struct file_operations usbc_vdm_fops = {
	.owner = THIS_MODULE,
	.open = usbc_vdm_open,
	.read = seq_read,
	.write = usbc_vdm_write_file,
	.llseek = seq_lseek,
	.release = single_release,
};

static void usbc_debugfs_init(struct max77705_usbc *u)
{
	u->debugfs = debugfs_create_dir("max77705-usbc", NULL);
	debugfs_create_file("events", 0400, u->debugfs, u, &usbc_events_fops);
	debugfs_create_file("registers", 0400, u->debugfs, u, &usbc_regs_fops);
	debugfs_create_file("pd", 0400, u->debugfs, u, &usbc_pd_fops);
	if (vdm_debugfs > 0 || vdm_discovery)
		debugfs_create_file("vdm", vdm_debugfs > 0 ? 0600 : 0400,
				    u->debugfs, u, &usbc_vdm_fops);
}

/* ---- probe ---------------------------------------------------------------- */

/* runs after the interrupt is freed and before the power_supply goes */
static void usbc_stop_work(void *data)
{
	struct max77705_usbc *u = data;

	WRITE_ONCE(u->stopping, true);
	debugfs_remove_recursive(u->debugfs);
	cancel_delayed_work_sync(&u->poll_work);
	cancel_work_sync(&u->event_work);
	cancel_delayed_work_sync(&u->hv_work);
}

static void usbc_destroy_wq(void *data)
{
	struct max77705_usbc *u = data;

	destroy_workqueue(u->wq);
}

static int max77705_usbc_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct power_supply_config psy_cfg = {};
	struct max77705_usbc *u;
	u8 rev[2], mask[4], minor = 0;
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
	mutex_init(&u->pub_lock);
	mutex_init(&u->vdm_lock);
	init_completion(&u->cmd_done);
	INIT_WORK(&u->event_work, usbc_event_work);
	INIT_DELAYED_WORK(&u->poll_work, usbc_poll_work);
	INIT_DELAYED_WORK(&u->hv_work, usbc_hv_work);
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
	ret = devm_add_action_or_reset(dev, usbc_destroy_wq, u);
	if (ret)
		return ret;

	if (!usbc_read(u, REG_UIC_INT_M, mask, 4))
		dev_info(dev, "masks were %02x %02x %02x %02x\n",
			 mask[0], mask[1], mask[2], mask[3]);
	ret = usbc_set_masks(u);
	if (ret)
		return dev_err_probe(dev, ret, "mask write failed\n");

	usbc_compute(u, &u->pub);
	psy_cfg.drv_data = u;
	psy_cfg.fwnode = dev_fwnode(dev);
	psy_cfg.attr_grp = usbc_psy_groups;
	/* by name, not a power-supplies phandle on the charger node: the
	 * charger must not wait for this driver to probe */
	psy_cfg.supplied_to = usbc_supplied_to;
	psy_cfg.num_supplicants = ARRAY_SIZE(usbc_supplied_to);
	u->psy = devm_power_supply_register(dev, &usbc_psy_desc, &psy_cfg);
	if (IS_ERR(u->psy))
		return dev_err_probe(dev, PTR_ERR(u->psy), "power_supply\n");

	usbc_typec_init(u);	/* optional: charging does not need it */
	usbc_debugfs_init(u);
	ret = devm_add_action_or_reset(dev, usbc_stop_work, u);
	if (ret)
		return ret;

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
