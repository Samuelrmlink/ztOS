// SPDX-License-Identifier: GPL-2.0
/*
 * Exynos 9810 DisplayPort TX over USB-C: the combo PHY in DP mode and the
 * DP link controller's AUX channel (M2b of docs/dp-altmode.md).
 *
 * The SoC has Samsung's own DP link controller (0x11090000 on the 9810; no
 * mainline driver matches it) and a USB3/DP combo PHY whose PMA sits at
 * 0x110a0000, with its power and reset bits in the USB PHY's USBCON block.
 * Register sequences are Samsung's 4.9 kernel (dpu_9810/displayport_reg.c,
 * phy/phy-exynos-usbdp.c, phy-exynos-usb3p1.c; GPL-2.0), transcribed step by
 * step; file:line references in the comments.
 *
 * The board's wiring comes from the device tree only (register ranges, the
 * USB PHY node, the PMU isolation bit, the lane maps per cable orientation);
 * without it the driver does not bind. In the typec graph the node is a
 * mode-switch and orientation-switch on the connector's SuperSpeed port, so
 * the class tells it the DP pin assignment (C/D/E), the orientation and,
 * through typec_displayport's data, the partner's DP Status (HPD).
 *
 * Nothing touches the hardware at probe. Bring-up is staged and, for now,
 * run by hand through debugfs exynos9810-dp/step (one phase per write), so
 * the first run on the phone can stop at the first phase that misbehaves:
 *
 *   id     read the link controller's version register (the first access
 *          to 0x11090000 at all), only once x9dp_link_ok() has seen the
 *          link block powered and clocked
 *   pma    power the combo PMA up and run its common init
 *   phy    the PMA in DP mode for the pin assignment and orientation the
 *          typec class gave (lanes, PLL lock, AUX TX level)
 *   ctrl   link controller reset, PCS + AUX functions on, lane map
 *   dpcd   AUX: read DPCD 0x000-0x00f (receiver capabilities)
 *   edid   AUX: read EDID block 0 over I2C-over-AUX
 *   train  link training at link_rate (default HBR) on the pin assignment's
 *          lanes: clock recovery then channel EQ, lower rates on failure
 *   bist   1080p60 colour bars from the link controller's own pattern
 *          generator (no DECON): video timing, Mvid/Nvid, transfer unit,
 *          BIST on, VIDEO_EN
 *   video_off  VIDEO_EN, BIST and the SST1 video function off
 *   status read only: the sink's DPCD 0x200-0x207 and symbol error
 *          counters, the controller's SST1 function/interrupt/debug state,
 *          logged and kept for debugfs state
 *   lh     the SST1 LH power channel towards the DPU (stock powers it as
 *          DECON2 starts); by hand only, never in the automatic path
 *   m4dump read only: DECON2, DMA and DPP channel 1, the DMA common block
 *          (M4, the DECON2 framebuffer), after checking the DPU power domain,
 *          DECON2's APB clock and the DPU/DMA/DPP Q-channels read on;
 *          debugfs m4regs shows it. Ranges from the DT (optional: without
 *          them the step says so and touches nothing)
 *   off    (decon2_stop if DECON2 was started), AUX/PCS off, PHY reset,
 *          PMA back to low power and reset
 *
 * M4, the approved writes (see the M4 section below), by hand only:
 *   route  W1 DECON0 DP_CONNECTION_CONTROL[2:0] := 2, W2 disp sysreg
 *          TE_QACTIVE_PLL_EN := 1; read back
 *   sram   W4 DECON2 SRAM1-3 (12K)
 *   decon2 DECON2 set up for 1080p video to DP, blender background bg_rgb
 *   video  the link's video from DECON2 (no BIST); after video_off
 *   decon2_start  DECON2 on, RUN_STATUS (needs lh, route, sram)
 *   decon2_stop   DECON2 off, LH off, DECON2 soft reset
 *   dma    W6 DMA/DPP channel 1 on a 1920x1080 test image (no window
 *          maps it to DECON2 yet)
 *
 * auto=1 runs pma..edid itself when the class enters a DP pin state, and
 * off when it leaves; default 0.
 *
 * hpd_read=1 (default 0, writable at run time): when the partner's DP Status
 * says HPD high (typec_displayport's data in the mux state, from an
 * Attention), the phases not yet done (id, pma, phy, ctrl), then dpcd and
 * edid, logged. HPD low changes nothing (the sink may raise it again; stock
 * would stop the link, which this driver does not start yet).
 *
 * pma_early=1 (default 0, writable at run time) runs only the pma phase when
 * the class enters a DP state, and off when it leaves. max77705-usbc sets a
 * DP state at Discover SVIDs, before Enter Mode, so this is stock's order:
 * Samsung's USB3 bring-up has the PMA up before the dock is asked to enter
 * DP (dp-hpd-research candidate 1). For a dock that raises HPD only then.
 *
 * Every combo PHY access first reads the PMU isolation bit the DT names
 * (PMU 0x704 bit 0 on the 9810, stock's USBDP_PHY_CONTROL check,
 * displayport.h:1076-1145): with the PHY isolated the access is skipped and
 * the phase fails, instead of hanging the bus.
 */
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>
#include <linux/workqueue.h>
#include <drm/display/drm_dp.h>
#include <drm/display/drm_dp_helper.h>	/* struct drm_dp_aux_msg only */
#include <drm/drm_edid.h>			/* DDC_ADDR, EDID_LENGTH */

/* ---- link controller (regs-displayport.h) ----------------------------------- */
#define SYSTEM_DEVICE_VERSION		0x0000
#define SYSTEM_SW_RESET_CONTROL		0x0004
#define  SW_RESET			BIT(0)
#define SYSTEM_SW_FUNCTION_ENABLE	0x0014
#define  SW_FUNC_EN			BIT(0)
#define SYSTEM_COMMON_FUNCTION_ENABLE	0x0018
#define  PCS_FUNC_EN			BIT(1)
#define  AUX_FUNC_EN			BIT(0)
#define SYSTEM_PLL_LOCK_CONTROL		0x002c
#define  PLL_LOCK_STATUS		BIT(4)
#define AUX_CONTROL			0x1000
#define  AUX_REPLY_TIMER_MODE		GENMASK(13, 12)
#define  AUX_TIMEOUT_1800US		3
#define AUX_TRANSACTION_START		0x1004
#define  AUX_TRAN_START			BIT(0)
#define AUX_BUFFER_CLEAR		0x1008
#define  AUX_BUF_CLR			BIT(0)
#define AUX_ADDR_ONLY_COMMAND		0x100c
#define  ADDR_ONLY_CMD			BIT(0)
#define AUX_REQUEST_CONTROL		0x1010
#define  REQ_COMM			GENMASK(31, 28)
#define  REQ_ADDR			GENMASK(27, 8)
#define  REQ_LENGTH			GENMASK(5, 0)
#define AUX_COMMAND_CONTROL		0x1014
#define  DEFER_CTRL_EN			BIT(8)
#define AUX_MONITOR_1			0x1018
#define  AUX_CMD_STATUS			GENMASK(11, 8)
#define AUX_MONITOR_2			0x101c
#define AUX_TX_DATA_SET0		0x1030
#define AUX_RX_DATA_SET0		0x1040
#define AUX_DATA_MAX			16	/* displayport.h:661 */
#define PCS_LANE_CONTROL		0x3004	/* LANEn_MAP at 4n */
/* M3 (regs-displayport.h:28, :374-395, :586-767) */
#define SYSTEM_MAIN_LINK_BANDWIDTH	0x000c
#define SYSTEM_SST1_FUNCTION_ENABLE	0x001c
#define  SST1_LH_PWR_ON_STATUS		BIT(5)
#define  SST1_LH_PWR_ON			BIT(4)
#define  SST1_VIDEO_FUNC_EN		BIT(0)
#define SYSTEM_DEBUG_LH_PCH		0x0204
#define DPCD_SYMBOL_ERROR_COUNT_LANE0	0x00210	/* DP 1.2: 0x210-0x217, lanes 0-3, 2 bytes each */
#define SST1_INTERRUPT_STATUS_SET0	0x5024	/* MAPI_FIFO_UNDER_FLOW b8, VSYNC_DET b7 */
#define SST1_INTERRUPT_STATUS_SET1	0x5028
#define SST1_VIDEO_DEBUG_FSM_STATE	0x5460
#define SST1_VIDEO_DEBUG_MAPI		0x5464
#define SYSTEM_MAIN_LINK_LANE_COUNT	0x0010
#define PCS_CONTROL			0x3000
#define  LINK_TRAINING_PATTERN_SET	GENMASK(5, 4)
#define  SCRAMBLE_BYPASS		BIT(0)
#define PCS_TEST_PATTERN_CONTROL	0x3008
#define  LINK_QUALITY_PATTERN_SET	GENMASK(2, 0)
#define SST1_MAIN_CONTROL		0x5000
#define  MVID_MODE			BIT(11)
#define  VIDEO_MODE			BIT(6)
#define  ENHANCED_MODE			BIT(5)
#define SST1_MVID_MASTER_MODE		0x5044
#define SST1_NVID_MASTER_MODE		0x5048
#define SST1_MVID_SFR_CONFIGURE		0x504c
#define SST1_NVID_SFR_CONFIGURE		0x5050
#define SST1_ACTIVE_SYMBOL_INTEGER_FEC_OFF	0x5080
#define SST1_ACTIVE_SYMBOL_FRACTION_FEC_OFF	0x5084
#define SST1_ACTIVE_SYMBOL_THRESHOLD_FEC_OFF	0x5088
#define SST1_ACTIVE_SYMBOL_THRESHOLD_SEL_FEC_OFF 0x508c
#define SST1_ACTIVE_SYMBOL_INTEGER_FEC_ON	0x5090
#define SST1_ACTIVE_SYMBOL_FRACTION_FEC_ON	0x5094
#define SST1_ACTIVE_SYMBOL_THRESHOLD_FEC_ON	0x5098
#define SST1_ACTIVE_SYMBOL_THRESHOLD_SEL_FEC_ON	0x509c
#define SST1_ACTIVE_SYMBOL_MODE_CONTROL	0x5104
#define SST1_VIDEO_CONTROL		0x5400
#define  STRM_VALID_FORCE		BIT(9)
#define  STRM_VALID_CTRL		BIT(8)
#define  DYNAMIC_RANGE_MODE		BIT(7)	/* 0 VESA (0-255), 1 CEA */
#define  VIDEO_BPC			GENMASK(6, 4)	/* 0 6 bpc, 1 8 bpc, 2 10 bpc */
#define  COLOR_FORMAT			GENMASK(3, 2)	/* 0 RGB */
#define  VSYNC_POLARITY			BIT(1)
#define  HSYNC_POLARITY			BIT(0)
#define SST1_VIDEO_ENABLE		0x5404
#define  VIDEO_EN			BIT(0)
#define SST1_VIDEO_MASTER_TIMING_GEN	0x5408
#define SST1_VIDEO_HORIZONTAL_TOTAL_PIXELS	0x5414
#define SST1_VIDEO_VERTICAL_TOTAL_PIXELS	0x5418
#define SST1_VIDEO_HORIZONTAL_FRONT_PORCH	0x541c
#define SST1_VIDEO_HORIZONTAL_BACK_PORCH	0x5420
#define SST1_VIDEO_HORIZONTAL_ACTIVE		0x5424
#define SST1_VIDEO_VERTICAL_FRONT_PORCH		0x5428
#define SST1_VIDEO_VERTICAL_BACK_PORCH		0x542c
#define SST1_VIDEO_VERTICAL_ACTIVE		0x5430
#define SST1_VIDEO_BIST_CONTROL		0x5450
#define  BIST_EN			BIT(3)
#define  BIST_TYPE			GENMASK(1, 0)	/* 0 colour bar, 1 WGB, 2 MW */

/* ---- combo PHY, DP side and common block (regs-displayport.h, phy-exynos-usbdp-reg.h) */
#define COM_CMN_R0E			0x0038
#define COM_CMN_R0F			0x003c
#define COM_CMN_R10			0x0040
#define COM_CMN_R24			0x0090
#define  CMN24_SSC_EN			BIT(0)
#define COM_CMN_R25			0x0094
#define COM_CMN_R26			0x0098
#define COM_CMN_R27			0x009c
#define CMN_REG2C			0x00b0
#define  MAN_USBDP_MODE			GENMASK(2, 1)
#define  MAN_USBDP_MODE_EN		BIT(0)
#define CMN_REG2D			0x00b4
#define  USB_TX1_SEL			BIT(5)
#define  USB_TX3_SEL			BIT(4)
#define  LCPLL_SSCDIV			GENMASK(3, 0)
#define COM_TRSV_R0D			0x00f4
#define COM_TRSV_R1F			0x013c
#define COM_TRSV_R21			0x0144
#define COM_TRSV_R23			0x014c
#define COM_TRSV_R24			0x0150
#define  TRSV24_MAN_TX_RCV_DET_EN	BIT(7)
#define COM_TRSV_R34			0x0190
#define COM_TRSV_R38			0x01a0
#define COM_TRSV_R3D			0x01b4
#define COM_TRSV_R56			0x0218
#define DP_REG_0			0x0800
#define  LN_LANE_EN(n)			BIT(3 - (n))	/* LN0 bit 3 .. LN3 bit 0 */
#define DP_REG_3			0x080c	/* TX amplitude level, 2 bits per lane, LN0 at 7:6 */
#define DP_REG_4			0x0810	/* TX pre-emphasis level, same layout */
#define DP_REG_1			0x0804
#define  CMN_INIT_RSTN			BIT(0)
#define DP_REG_B			0x082c
#define  LN_TXCLK_SOURCE_LANE		GENMASK(1, 0)
#define DP_REG_F			0x083c
#define  AUX_TX_LVL_CTRL		GENMASK(3, 0)
#define DP_REG_11			0x0844
#define DP_REG_16			0x0858
#define DP_REG_1A			0x0868
#define DP_REG_31			0x08c4
#define DP_REG_36			0x08d8
#define DP_REG_3A			0x08e8
#define DP_REG_76			0x09d8
#define DP_REG_7A			0x09e8
#define DP_REG_51			0x0944
#define DP_REG_56			0x0958
#define DP_REG_5A			0x0968
#define DP_REG_71			0x09c4
#define DP_REG_B3			0x0acc
#define  CMN_DUMMY_CTRL_7_6		GENMASK(7, 6)
#define  CMN_DUMMY_CTRL_1_0		GENMASK(1, 0)
#define DP_REG_C6			0x0b18
#define DP_REG_C9			0x0b24
#define DP_REG_CF			0x0b3c
#define DP_REG_D9			0x0b64
#define DP_REG_DF			0x0b7c
#define DP_REG_E9			0x0ba4
#define DP_REG_EF			0x0bbc

/* ---- USBCON (phy-exynos-usb3p1-reg.h): the PMA's power and resets ------------ */
#define USBCON_COMBO_PMA_CTRL		0x48
#define  PMA_REF_FREQ			GENMASK(9, 8)
#define  PMA_PLL_REF_REQ		GENMASK(11, 10)
#define  PMA_LOW_PWRN			BIT(4)
#define  PMA_TRSV_SW_RST		BIT(3)
#define  PMA_CMN_SW_RST			BIT(2)
#define  PMA_INIT_SW_RST		BIT(1)
#define  PMA_APB_SW_RST			BIT(0)
#define  PMA_ALL_RST			(PMA_TRSV_SW_RST | PMA_CMN_SW_RST | \
					 PMA_INIT_SW_RST | PMA_APB_SW_RST)

static bool auto_bringup;
module_param_named(auto, auto_bringup, bool, 0644);
MODULE_PARM_DESC(auto, "run the bring-up phases on a DP pin state, off on leaving it (default 0: debugfs only)");

static bool hpd_read;
module_param(hpd_read, bool, 0644);
MODULE_PARM_DESC(hpd_read, "on HPD high: bring-up phases as needed, then read DPCD and EDID (default 0)");

static bool hpd_bist;
module_param(hpd_bist, bool, 0644);
MODULE_PARM_DESC(hpd_bist, "with hpd_read: after DPCD/EDID, train the link and start the 1080p60 colour bars (default 0)");

static uint link_rate = DP_LINK_BW_2_7;
module_param(link_rate, uint, 0644);
MODULE_PARM_DESC(link_rate, "link training start rate, DPCD code: 0x06 RBR, 0x0a HBR (default), 0x14 HBR2");

static bool pma_early;
module_param(pma_early, bool, 0644);
MODULE_PARM_DESC(pma_early, "power the combo PMA up when a DP state is set (before Enter Mode), down when it ends (default 0)");

enum { ORIENT_NORMAL, ORIENT_REVERSE };

struct x9dp {
	struct device *dev;
	void __iomem *link;		/* DP link controller */
	void __iomem *phy;		/* combo PHY PMA (DP + common) */
	void __iomem *usbcon;		/* the USB PHY's USBCON block (not ours: two bits) */
	struct regmap *pmu;
	u32 iso_off, iso_mask;		/* PHY isolation bit: set = not isolated */
	u32 pd_off, pd_mask;		/* link power domain status: all mask bits = on */
	void __iomem *qch;		/* the link's clock Q-channels (CMU, read only) */
	u32 qch_n;
	u32 lane_map[2][4];		/* PCS lane map per orientation (DT) */
	u32 two_lanes[2][2];		/* PMA lanes for a 2-lane pin assignment (DT) */

	struct typec_switch_dev *sw;
	struct typec_mux_dev *mux;

	struct mutex lock;		/* everything below */
	enum typec_orientation orient;
	unsigned long mode;		/* typec mux state */
	u32 dp_status, dp_conf;
	bool pma_on, phy_on, ctrl_on;
	u32 pma_saved;			/* COMBO_PMA_CTRL as the USB driver left it */
	struct work_struct auto_work;
	struct work_struct hpd_work;
	bool id_done;
	struct dentry *dbg;
	char last[320];
	u8 dpcd[16];
	bool dpcd_ok;
	u8 edid[EDID_LENGTH];
	bool edid_ok;
	u8 rate, nlanes;		/* trained link, 0: none */
	bool video_on;
	char vstat[320];		/* the last status step */
	/* M4 (DT, optional; mapped, not requested: DECON0's and the DMA's
	 * owners are other drivers) */
	void __iomem *decon2, *dma1, *dpp1, *dma_com, *dpu_cmu;
	u32 decon2_sz, dma1_sz, dpp1_sz, dma_com_sz;
	char *m4buf;
	size_t m4len;
	/* M4 writes: DECON0's DP_CONNECTION_CONTROL (and its shadow, read
	 * only), the disp sysreg's TE_QACTIVE_PLL_EN; mapped, not requested */
	void __iomem *d0conn, *d0conn_shd, *dsys;
	bool vsrc_decon;		/* video_on with DECON2's pixels, not BIST */
	bool decon2_on;
	void *fb_cpu;			/* the dma step's test image */
	dma_addr_t fb_dma;
	size_t fb_sz;
};

/* ---- register access ----------------------------------------------------------- */

static bool x9dp_phy_ok(struct x9dp *d)
{
	u32 v = 0;

	if (regmap_read(d->pmu, d->iso_off, &v) || !(v & d->iso_mask)) {
		dev_warn_ratelimited(d->dev, "combo PHY isolated (PMU %#x = %08x): not touched\n",
				     d->iso_off, v);
		return false;
	}
	return true;
}

/*
 * Read only, before the first access to the link controller: its power
 * domain on (PMU status, all mask bits set: pd-dpu on the 9810) and every
 * clock Q-channel either in hardware control (ENABLE, bit 0: the clock is
 * requested on access) or with its clock request forced (CLOCK_REQ, bit 1).
 * Read on the phone before this driver existed: pd-dpu 0x0010000f, both
 * DP_LINK Q-channels 0x003f0002 (forced on).
 */
static bool x9dp_link_ok(struct x9dp *d)
{
	u32 v = 0, i;

	if (regmap_read(d->pmu, d->pd_off, &v) || (v & d->pd_mask) != d->pd_mask) {
		dev_warn(d->dev, "link power domain off (PMU %#x = %08x): link not touched\n",
			 d->pd_off, v);
		return false;
	}
	for (i = 0; i < d->qch_n; i++) {
		v = readl(d->qch + 4 * i);
		if (!(v & (BIT(0) | BIT(1)))) {
			dev_warn(d->dev, "link clock Q-channel %u off (%08x): link not touched\n", i, v);
			return false;
		}
	}
	return true;
}

static int phy_wr(struct x9dp *d, u32 reg, u32 val)
{
	if (!x9dp_phy_ok(d))
		return -EIO;
	writel(val, d->phy + reg);
	return 0;
}

static int phy_rmw(struct x9dp *d, u32 reg, u32 mask, u32 val)
{
	u32 v;

	if (!x9dp_phy_ok(d))
		return -EIO;
	v = readl(d->phy + reg);
	writel((v & ~mask) | (val & mask), d->phy + reg);
	return 0;
}

static void link_rmw(struct x9dp *d, u32 reg, u32 mask, u32 val)
{
	u32 v = readl(d->link + reg);

	writel((v & ~mask) | (val & mask), d->link + reg);
}

/* ---- phases ---------------------------------------------------------------------- */

static int x9dp_lanes(struct x9dp *d)
{
	switch (d->mode) {
	case TYPEC_DP_STATE_C:
	case TYPEC_DP_STATE_E:
		return 4;
	case TYPEC_DP_STATE_D:
		return 2;
	default:
		return 0;
	}
}

static int x9dp_oidx(struct x9dp *d)
{
	return d->orient == TYPEC_ORIENTATION_REVERSE ? ORIENT_REVERSE : ORIENT_NORMAL;
}

static int x9dp_id(struct x9dp *d)
{
	u32 v;

	if (!x9dp_link_ok(d))
		return -EIO;
	v = readl(d->link + SYSTEM_DEVICE_VERSION);
	d->id_done = true;
	scnprintf(d->last, sizeof(d->last), "id: SYSTEM_DEVICE_VERSION %08x", v);
	return 0;
}

/*
 * The PMA up: phy-exynos-usb3p1.c:136-184 (pma_ready, pma_sw_rst_release)
 * around phy-exynos-usbdp.c:359-494 (the common block's init), as stock's
 * dwc3_exynos_phy_enable() runs them before every DP bring-up
 * (displayport_reg.c:1307-1318). Left out on purpose, because the USB 2.0
 * link shares USBCON with us: LINK_CTRL's PIPE3 override and CLKRST's pipe3
 * pclk select (the USB driver keeps the link on its UTMI clock), and the
 * USB-only PCS reset and SS tune values.
 */
static int x9dp_pma(struct x9dp *d)
{
	u32 v;
	int ret;

	if (!x9dp_phy_ok(d))
		return -EIO;
	v = readl(d->usbcon + USBCON_COMBO_PMA_CTRL);
	if (!d->pma_on)
		d->pma_saved = v;
	v &= ~PMA_LOW_PWRN;
	writel(v, d->usbcon + USBCON_COMBO_PMA_CTRL);
	udelay(1);
	v |= PMA_ALL_RST;
	writel(v, d->usbcon + USBCON_COMBO_PMA_CTRL);
	udelay(1);
	v &= ~(PMA_APB_SW_RST | PMA_INIT_SW_RST | PMA_PLL_REF_REQ | PMA_REF_FREQ);
	v |= FIELD_PREP(PMA_REF_FREQ, 1);
	writel(v, d->usbcon + USBCON_COMBO_PMA_CTRL);

	/* common block (usbdp.c:363-430), receiver detection off meanwhile */
	ret = phy_rmw(d, COM_TRSV_R24, TRSV24_MAN_TX_RCV_DET_EN, TRSV24_MAN_TX_RCV_DET_EN);
	ret = ret ?: phy_wr(d, COM_CMN_R0E, (2 << 5) | 0x4);
	ret = ret ?: phy_wr(d, COM_CMN_R0F, BIT(7) | (0xf << 3) | 0x7);
	ret = ret ?: phy_wr(d, COM_CMN_R10, (6 << 5) | 0x8);
	ret = ret ?: phy_wr(d, COM_CMN_R25, 0x30);
	ret = ret ?: phy_wr(d, COM_CMN_R26, BIT(6) | BIT(3) | BIT(2) | 0x3);
	ret = ret ?: phy_wr(d, COM_CMN_R27, BIT(7) | (5 << 4) | BIT(3) | 0x4);
	ret = ret ?: phy_wr(d, COM_TRSV_R23, BIT(7) | 0x5);
	ret = ret ?: phy_rmw(d, COM_CMN_R24, CMN24_SSC_EN, CMN24_SSC_EN);
	/*
	 * The lane mux for the cable orientation (usbdp.c:415-449, used_phy_port
	 * = !gpg0-4): the same values as a 2-lane DP mode on the PMA lanes the
	 * DT gives for this orientation, plus the lower DP TX level on the two
	 * lanes DP does not get.
	 */
	if (d->two_lanes[x9dp_oidx(d)][0] == 2) {
		ret = ret ?: phy_rmw(d, CMN_REG2C, MAN_USBDP_MODE | MAN_USBDP_MODE_EN,
				     FIELD_PREP(MAN_USBDP_MODE, 3) | MAN_USBDP_MODE_EN);
		ret = ret ?: phy_rmw(d, CMN_REG2D, USB_TX1_SEL | USB_TX3_SEL, USB_TX1_SEL);
		ret = ret ?: phy_rmw(d, DP_REG_B3, CMN_DUMMY_CTRL_7_6 | CMN_DUMMY_CTRL_1_0,
				     FIELD_PREP(CMN_DUMMY_CTRL_7_6, 1) |
				     FIELD_PREP(CMN_DUMMY_CTRL_1_0, 3));
		ret = ret ?: phy_wr(d, DP_REG_16, 0);
		ret = ret ?: phy_wr(d, DP_REG_1A, 0);
	} else {
		ret = ret ?: phy_rmw(d, CMN_REG2C, MAN_USBDP_MODE | MAN_USBDP_MODE_EN,
				     MAN_USBDP_MODE_EN);
		ret = ret ?: phy_rmw(d, CMN_REG2D, USB_TX1_SEL | USB_TX3_SEL, USB_TX3_SEL);
		ret = ret ?: phy_rmw(d, DP_REG_B3, CMN_DUMMY_CTRL_7_6 | CMN_DUMMY_CTRL_1_0,
				     FIELD_PREP(CMN_DUMMY_CTRL_7_6, 2));
		ret = ret ?: phy_wr(d, DP_REG_56, 0);
		ret = ret ?: phy_wr(d, DP_REG_5A, 0);
	}
	ret = ret ?: phy_rmw(d, CMN_REG2D, LCPLL_SSCDIV, FIELD_PREP(LCPLL_SSCDIV, 1));
	ret = ret ?: phy_wr(d, COM_TRSV_R38, (2 << 5) | (2 << 2) | BIT(0));
	ret = ret ?: phy_rmw(d, COM_TRSV_R1F, BIT(0), BIT(0));
	ret = ret ?: phy_wr(d, COM_TRSV_R21, (1 << 4) | 0x2);
	ret = ret ?: phy_rmw(d, COM_TRSV_R0D, BIT(0), BIT(0));
	ret = ret ?: phy_rmw(d, COM_TRSV_R56, BIT(0), BIT(0));
	ret = ret ?: phy_wr(d, COM_TRSV_R34, (3 << 2) | BIT(1) | BIT(0));
	ret = ret ?: phy_rmw(d, COM_TRSV_R24, TRSV24_MAN_TX_RCV_DET_EN, 0);
	ret = ret ?: phy_wr(d, COM_TRSV_R3D, 0x78);
	if (ret)
		return ret;

	/* CMN and TRSV out of reset; the PMA settles in 200 us (usb3p1.c:172-178) */
	v &= ~(PMA_CMN_SW_RST | PMA_TRSV_SW_RST);
	writel(v, d->usbcon + USBCON_COMBO_PMA_CTRL);
	udelay(200);
	d->pma_on = true;
	scnprintf(d->last, sizeof(d->last), "pma: on (COMBO_PMA_CTRL %08x, was %08x)",
		  readl(d->usbcon + USBCON_COMBO_PMA_CTRL), d->pma_saved);
	return 0;
}

/*
 * DP mode: displayport_reg_phy_init() (displayport_reg.c:1307-1318): CMN
 * reset, the fixed init values (:142-155), the lane mode for the pin
 * assignment and orientation (:157-229), reset release, PLL lock (:236-249,
 * read in the link controller), AUX TX level (:1302-1305). Which PMA lanes
 * carry a 2-lane assignment is the board's (DT); the register values for
 * "lanes 0+1" and "lanes 2+3" are the PHY's.
 */
/* the PHY in DP mode, left in CMN reset (the caller sets the link rate, then releases) */
static int x9dp_phy_program(struct x9dp *d, u32 *lane_en)
{
	static const u32 init[][2] = {
		{ DP_REG_11, 0x00 }, { DP_REG_31, 0x00 }, { DP_REG_51, 0x00 },
		{ DP_REG_71, 0x00 }, { DP_REG_C6, 0x20 }, { DP_REG_C9, 0x1e },
		{ DP_REG_D9, 0x30 }, { DP_REG_E9, 0x30 }, { DP_REG_CF, 0x0d },
		{ DP_REG_DF, 0x08 }, { DP_REG_EF, 0x08 },
	};
	int lanes = x9dp_lanes(d), o = x9dp_oidx(d), i, ret;
	u32 mode, tx, b3_10, b3_76, en, clk;

	if (!d->pma_on || !d->id_done)
		return -EAGAIN;		/* pma first; id: the link is read for PLL lock */
	if (!lanes)
		return -ENOLINK;	/* no DP pin state from the class yet */
	if (lanes == 4) {
		mode = 2; tx = 0; b3_10 = 2; b3_76 = 0; clk = 0;
		en = LN_LANE_EN(0) | LN_LANE_EN(1) | LN_LANE_EN(2) | LN_LANE_EN(3);
	} else if (d->two_lanes[o][0] == 2 && d->two_lanes[o][1] == 3) {
		mode = 3; tx = USB_TX1_SEL; b3_10 = 3; b3_76 = 1; clk = 3;
		en = LN_LANE_EN(2) | LN_LANE_EN(3);
	} else {
		mode = 0; tx = USB_TX3_SEL; b3_10 = 0; b3_76 = 2; clk = 0;
		en = LN_LANE_EN(0) | LN_LANE_EN(1);
	}

	/* reset, init values, lane mode (displayport_reg.c:129-229) */
	ret = phy_rmw(d, DP_REG_1, CMN_INIT_RSTN, 0);
	for (i = 0; !ret && i < ARRAY_SIZE(init); i++)
		ret = phy_wr(d, init[i][0], init[i][1]);
	ret = ret ?: phy_rmw(d, CMN_REG2C, MAN_USBDP_MODE | MAN_USBDP_MODE_EN,
			     FIELD_PREP(MAN_USBDP_MODE, mode) | MAN_USBDP_MODE_EN);
	ret = ret ?: phy_rmw(d, CMN_REG2D, USB_TX1_SEL | USB_TX3_SEL, tx);
	ret = ret ?: phy_rmw(d, DP_REG_B3, CMN_DUMMY_CTRL_1_0 | CMN_DUMMY_CTRL_7_6,
			     FIELD_PREP(CMN_DUMMY_CTRL_1_0, b3_10) |
			     FIELD_PREP(CMN_DUMMY_CTRL_7_6, b3_76));
	ret = ret ?: phy_rmw(d, DP_REG_B, LN_TXCLK_SOURCE_LANE, clk);
	ret = ret ?: phy_wr(d, DP_REG_0, en | 0xf0);
	if (ret)
		return ret;
	*lane_en = en;
	return 0;
}

static int x9dp_phy(struct x9dp *d)
{
	int lanes = x9dp_lanes(d), o = x9dp_oidx(d), ret;
	u32 en, v;

	ret = x9dp_phy_program(d, &en);
	ret = ret ?: phy_rmw(d, DP_REG_1, CMN_INIT_RSTN, CMN_INIT_RSTN);
	if (ret)
		return ret;
	ret = readl_poll_timeout(d->link + SYSTEM_PLL_LOCK_CONTROL, v, v & PLL_LOCK_STATUS,
				 1, 300);
	if (ret)
		dev_warn(d->dev, "PHY PLL not locked in 300 us (%08x)\n", v);
	ret = phy_rmw(d, DP_REG_F, AUX_TX_LVL_CTRL, 0xf);
	if (ret)
		return ret;
	d->phy_on = true;
	scnprintf(d->last, sizeof(d->last), "phy: DP mode, %d lanes (%s), DP_REG_0 %02x, PLL %s",
		  lanes, o ? "reverse" : "normal", en | 0xf0, v & PLL_LOCK_STATUS ? "locked" : "NOT locked");
	return 0;
}

/*
 * The link controller: sw reset (displayport_reg.c:110-127), PCS and AUX
 * functions and the software function (:432-437, :1294-1300; the SST1 video
 * function stays off until there is video), the lane map (:1193-1238, the
 * board's per orientation from the DT).
 */
static int x9dp_ctrl(struct x9dp *d)
{
	const u32 *m = d->lane_map[x9dp_oidx(d)];
	u32 v;
	int ret;

	if (!d->phy_on)
		return -EAGAIN;
	writel(SW_RESET, d->link + SYSTEM_SW_RESET_CONTROL);
	ret = readl_poll_timeout(d->link + SYSTEM_SW_RESET_CONTROL, v, !(v & SW_RESET), 1, 10);
	if (ret)
		dev_warn(d->dev, "link sw reset did not clear (%08x)\n", v);
	link_rmw(d, SYSTEM_COMMON_FUNCTION_ENABLE, PCS_FUNC_EN | AUX_FUNC_EN,
		 PCS_FUNC_EN | AUX_FUNC_EN);
	link_rmw(d, SYSTEM_SW_FUNCTION_ENABLE, SW_FUNC_EN, SW_FUNC_EN);
	link_rmw(d, PCS_LANE_CONTROL, 0x3333,
		 m[0] | m[1] << 4 | m[2] << 8 | m[3] << 12);
	d->ctrl_on = true;
	scnprintf(d->last, sizeof(d->last), "ctrl: on, lane map %u%u%u%u", m[0], m[1], m[2], m[3]);
	return 0;
}

/*
 * One AUX transaction, in drm_dp_aux's transfer form so it plugs into a
 * drm_dp_aux once there is a DRM device (M3). REQ_COMM is the DP request
 * code as is (native 8/9, I2C 0/1, MOT 4: displayport.h:101-106 uses the
 * same values). As stock (displayport_reg.c:827-866): defer handled in
 * hardware, 1800 us reply timer, start, poll, then any error status or
 * error count is a failure. Address-only: ADDR_ONLY_CMD.
 */
static ssize_t x9dp_aux_transfer(struct x9dp *d, struct drm_dp_aux_msg *msg)
{
	u8 *buf = msg->buffer;
	u32 req = msg->request & 0xf, v, m1, m2;
	bool rd = req & DP_AUX_I2C_READ;	/* bit 0: read, native or I2C */
	size_t i;
	int ret;

	if (!d->ctrl_on)
		return -ENODEV;
	if (msg->size > AUX_DATA_MAX)
		return -E2BIG;
	writel(AUX_BUF_CLR, d->link + AUX_BUFFER_CLEAR);
	link_rmw(d, AUX_COMMAND_CONTROL, DEFER_CTRL_EN, DEFER_CTRL_EN);
	link_rmw(d, AUX_CONTROL, AUX_REPLY_TIMER_MODE,
		 FIELD_PREP(AUX_REPLY_TIMER_MODE, AUX_TIMEOUT_1800US));
	writel(msg->size ? 0 : ADDR_ONLY_CMD, d->link + AUX_ADDR_ONLY_COMMAND);
	writel(FIELD_PREP(REQ_COMM, req) | FIELD_PREP(REQ_ADDR, msg->address) |
	       FIELD_PREP(REQ_LENGTH, msg->size ? msg->size - 1 : 0),
	       d->link + AUX_REQUEST_CONTROL);
	if (!rd)
		for (i = 0; i < msg->size; i += 4) {
			u32 w = 0;
			size_t k;

			for (k = 0; k < 4 && i + k < msg->size; k++)
				w |= buf[i + k] << (8 * k);
			writel(w, d->link + AUX_TX_DATA_SET0 + i);
		}
	writel(AUX_TRAN_START, d->link + AUX_TRANSACTION_START);
	ret = readl_poll_timeout(d->link + AUX_TRANSACTION_START, v, !(v & AUX_TRAN_START),
				 10, 50000);
	writel(0, d->link + AUX_ADDR_ONLY_COMMAND);
	if (ret)
		return -ETIMEDOUT;
	m1 = readl(d->link + AUX_MONITOR_1);
	m2 = readl(d->link + AUX_MONITOR_2);
	if ((m1 & AUX_CMD_STATUS) || m2) {
		dev_dbg(d->dev, "AUX %x@%05x: monitor %08x %08x\n", req, msg->address, m1, m2);
		return -EIO;
	}
	if (rd)
		for (i = 0; i < msg->size; i++)
			buf[i] = readl(d->link + AUX_RX_DATA_SET0 + (i & ~3)) >> (8 * (i & 3));
	msg->reply = (req & DP_AUX_NATIVE_WRITE) ? DP_AUX_NATIVE_REPLY_ACK : DP_AUX_I2C_REPLY_ACK;
	return msg->size;
}

static int x9dp_aux(struct x9dp *d, u8 request, u32 addr, void *buf, size_t len)
{
	struct drm_dp_aux_msg msg = {
		.request = request, .address = addr, .buffer = buf, .size = len,
	};
	int tries = 3, ret;	/* AUX_RETRY_COUNT, displayport.h:663 */

	do {
		ret = x9dp_aux_transfer(d, &msg);
	} while (ret < 0 && ret != -ENODEV && ret != -E2BIG && --tries);
	return ret < 0 ? ret : 0;
}

static int x9dp_dpcd(struct x9dp *d)
{
	int ret = x9dp_aux(d, DP_AUX_NATIVE_READ, DP_DPCD_REV, d->dpcd, sizeof(d->dpcd));

	d->dpcd_ok = !ret;
	if (ret)
		return ret;
	scnprintf(d->last, sizeof(d->last), "dpcd: rev %x.%x, max rate %02x, max lanes %u",
		  d->dpcd[DP_DPCD_REV] >> 4, d->dpcd[DP_DPCD_REV] & 0xf, d->dpcd[DP_MAX_LINK_RATE],
		  d->dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK);
	return 0;
}

/* EDID block 0: offset 0 to 0x50, 16-byte reads with MOT, then a stop */
static int x9dp_edid(struct x9dp *d)
{
	u8 off = 0;
	int i, ret;

	d->edid_ok = false;
	ret = x9dp_aux(d, DP_AUX_I2C_WRITE | DP_AUX_I2C_MOT, DDC_ADDR, &off, 1);
	for (i = 0; !ret && i < EDID_LENGTH; i += AUX_DATA_MAX)
		ret = x9dp_aux(d, DP_AUX_I2C_READ | DP_AUX_I2C_MOT, DDC_ADDR, d->edid + i,
			       AUX_DATA_MAX);
	x9dp_aux(d, DP_AUX_I2C_READ, DDC_ADDR, NULL, 0);	/* address-only: stop */
	if (ret)
		return ret;
	d->edid_ok = true;
	scnprintf(d->last, sizeof(d->last), "edid: header %8phN, manufacturer %02x%02x, product %02x%02x",
		  d->edid, d->edid[8], d->edid[9], d->edid[11], d->edid[10]);
	return 0;
}

/* back down: displayport_reg_deinit / phy_disable (:1320-1346), PMA as found */
static int x9dp_video_off(struct x9dp *d);
static int x9dp_decon2_stop(struct x9dp *d);

static int x9dp_off(struct x9dp *d)
{
	if (d->decon2_on)
		x9dp_decon2_stop(d);
	if (d->video_on)
		x9dp_video_off(d);
	d->rate = 0;
	if (d->ctrl_on) {
		link_rmw(d, SYSTEM_COMMON_FUNCTION_ENABLE, PCS_FUNC_EN | AUX_FUNC_EN, 0);
		link_rmw(d, SYSTEM_SW_FUNCTION_ENABLE, SW_FUNC_EN, 0);
		d->ctrl_on = false;
	}
	if (d->phy_on) {
		phy_rmw(d, DP_REG_1, CMN_INIT_RSTN, 0);
		phy_wr(d, DP_REG_0, 0);
		d->phy_on = false;
	}
	if (d->pma_on && x9dp_phy_ok(d)) {
		u32 v = readl(d->usbcon + USBCON_COMBO_PMA_CTRL);

		writel(v | PMA_ALL_RST, d->usbcon + USBCON_COMBO_PMA_CTRL);
		udelay(1);
		writel(d->pma_saved, d->usbcon + USBCON_COMBO_PMA_CTRL);
		d->pma_on = false;
	}
	d->dpcd_ok = d->edid_ok = false;
	scnprintf(d->last, sizeof(d->last), "off");
	return 0;
}

/* ---- M3: link training ------------------------------------------------------------ */

/*
 * TX swing/pre-emphasis per level, stock displayport_reg.c:20-25:
 * [swing][pre-emphasis] = { amplitude, emphasis, driver current enable }.
 */
static const u8 phy_tune[4][4][3] = {
	{ {4, 0, 0}, {0, 7, 0}, {2,  9, 1}, {0, 13, 1} },
	{ {2, 0, 0}, {2, 6, 1}, {0, 10, 1}, {0, 10, 1} },
	{ {2, 0, 1}, {0, 6, 1}, {0,  6, 1}, {0,  6, 1} },
	{ {0, 0, 1}, {0, 0, 1}, {0,  0, 1}, {0,  0, 1} },
};

static u32 x9dp_ls_khz(u8 rate)
{
	return rate == DP_LINK_BW_5_4 ? 540000 : rate == DP_LINK_BW_2_7 ? 270000 : 162000;
}

/*
 * The levels the sink asked for, on the PMA lanes that carry each link lane
 * (the DT's lane map for this orientation): stock set_phy_tune and
 * set_voltage_and_pre_emphasis (displayport_reg.c:302-430). The amplitude
 * and emphasis tables per PMA lane are at DP_REG_16/1A + 0x20 * lane.
 */
static int x9dp_levels(struct x9dp *d, const u8 *swing, const u8 *pre)
{
	static const u32 amp[4] = { DP_REG_16, DP_REG_36, DP_REG_56, DP_REG_76 };
	static const u32 emp[4] = { DP_REG_1A, DP_REG_3A, DP_REG_5A, DP_REG_7A };
	const u32 *map = d->lane_map[x9dp_oidx(d)];
	u32 r3 = 0, r4 = 0;
	int i, ret = 0;

	for (i = 0; !ret && i < d->nlanes; i++) {
		const u8 *t = phy_tune[swing[i]][pre[i]];
		u32 pma = map[i];

		ret = phy_wr(d, amp[pma] + swing[i] * 4, t[0] | t[2] << 4);
		ret = ret ?: phy_wr(d, emp[pma] + pre[i] * 4, t[1]);
	}
	for (i = 0; i < d->nlanes; i++) {
		r3 |= swing[i] << (6 - 2 * i);
		r4 |= pre[i] << (6 - 2 * i);
	}
	ret = ret ?: phy_wr(d, DP_REG_3, r3);
	return ret ?: phy_wr(d, DP_REG_4, r4);
}

static int x9dp_dpcd_wr(struct x9dp *d, u32 addr, const u8 *buf, size_t len)
{
	return x9dp_aux(d, DP_AUX_NATIVE_WRITE, addr, (void *)buf, len);
}

static int x9dp_dpcd_rd(struct x9dp *d, u32 addr, u8 *buf, size_t len)
{
	return x9dp_aux(d, DP_AUX_NATIVE_READ, addr, buf, len);
}

static void x9dp_pattern(struct x9dp *d, u8 tps)
{
	/* displayport_reg.c:271-280 */
	link_rmw(d, PCS_TEST_PATTERN_CONTROL, LINK_QUALITY_PATTERN_SET, 0);
	link_rmw(d, PCS_CONTROL, LINK_TRAINING_PATTERN_SET | SCRAMBLE_BYPASS,
		 FIELD_PREP(LINK_TRAINING_PATTERN_SET, tps) | (tps ? SCRAMBLE_BYPASS : 0));
}

/*
 * One rate: the PHY re-programmed with the rate and lane count set while its
 * common block is in reset, PLL lock, LINK_BW_SET (displayport_drv.c:301-345),
 * clock recovery with TPS1 (:347-430), channel EQ with TPS2/3 (:460-628).
 * Levels as the sink requests them in ADJUST_REQUEST.
 */
static int x9dp_train_at(struct x9dp *d, u8 rate, u8 lanes, bool enh, bool tps3, u32 rd_us)
{
	u8 swing[4] = {}, pre[4] = {}, set[4], st[3], adj[2], v[2], same = 0;
	int i, tries, ret;
	u32 en, pll;
	bool ok = false;

	d->nlanes = lanes;
	ret = x9dp_phy_program(d, &en);
	if (ret)
		return ret;
	writel(rate, d->link + SYSTEM_MAIN_LINK_BANDWIDTH);
	writel(lanes, d->link + SYSTEM_MAIN_LINK_LANE_COUNT);
	link_rmw(d, SST1_MAIN_CONTROL, ENHANCED_MODE, enh ? ENHANCED_MODE : 0);
	udelay(60);
	ret = phy_rmw(d, DP_REG_1, CMN_INIT_RSTN, CMN_INIT_RSTN);
	if (ret)
		return ret;
	if (readl_poll_timeout(d->link + SYSTEM_PLL_LOCK_CONTROL, pll, pll & PLL_LOCK_STATUS, 1, 300))
		dev_warn(d->dev, "train: PLL not locked at rate %02x\n", rate);

	v[0] = rate;
	v[1] = lanes | (enh ? DP_LANE_COUNT_ENHANCED_FRAME_EN : 0);
	ret = x9dp_dpcd_wr(d, DP_LINK_BW_SET, v, 2);
	if (ret)
		return ret;

	/* clock recovery */
	x9dp_pattern(d, 1);
	v[0] = DP_TRAINING_PATTERN_1 | DP_LINK_SCRAMBLING_DISABLE;
	ret = x9dp_dpcd_wr(d, DP_TRAINING_PATTERN_SET, v, 1);
	for (tries = 0; !ret && tries < 16; tries++) {
		ret = x9dp_levels(d, swing, pre);
		for (i = 0; i < 4; i++)
			set[i] = swing[i] | pre[i] << DP_TRAIN_PRE_EMPHASIS_SHIFT |
				 (swing[i] == 3 ? DP_TRAIN_MAX_SWING_REACHED : 0) |
				 (pre[i] == 3 ? DP_TRAIN_MAX_PRE_EMPHASIS_REACHED : 0);
		ret = ret ?: x9dp_dpcd_wr(d, DP_TRAINING_LANE0_SET, set, lanes);
		usleep_range(rd_us, rd_us + 100);
		ret = ret ?: x9dp_dpcd_rd(d, DP_LANE0_1_STATUS, st, 2);
		ret = ret ?: x9dp_dpcd_rd(d, DP_ADJUST_REQUEST_LANE0_1, adj, 2);
		if (ret)
			break;
		for (ok = true, i = 0; i < lanes; i++)
			ok &= !!(st[i / 2] >> (4 * (i & 1)) & DP_LANE_CR_DONE);
		dev_info(d->dev, "train %02x CR: status %02x %02x, adjust %02x %02x\n",
			 rate, st[0], st[1], adj[0], adj[1]);
		if (ok)
			break;
		for (same++, i = 0; i < lanes; i++) {
			u8 a = adj[i / 2] >> (4 * (i & 1));

			if ((a & 3) != swing[i])
				same = 0;
			swing[i] = a & 3;
			pre[i] = (a >> 2) & 3;
		}
		if (same >= 5 || swing[0] == 3)
			break;
	}
	if (ret || !ok)
		return ret ?: -EAGAIN;

	/* channel EQ */
	x9dp_pattern(d, tps3 ? 3 : 2);
	v[0] = (tps3 ? DP_TRAINING_PATTERN_3 : DP_TRAINING_PATTERN_2) | DP_LINK_SCRAMBLING_DISABLE;
	ret = x9dp_dpcd_wr(d, DP_TRAINING_PATTERN_SET, v, 1);
	for (tries = 0; !ret && tries < 6; tries++) {
		ret = x9dp_levels(d, swing, pre);
		for (i = 0; i < 4; i++)
			set[i] = swing[i] | pre[i] << DP_TRAIN_PRE_EMPHASIS_SHIFT |
				 (swing[i] == 3 ? DP_TRAIN_MAX_SWING_REACHED : 0) |
				 (pre[i] == 3 ? DP_TRAIN_MAX_PRE_EMPHASIS_REACHED : 0);
		ret = ret ?: x9dp_dpcd_wr(d, DP_TRAINING_LANE0_SET, set, lanes);
		usleep_range(rd_us, rd_us + 100);
		ret = ret ?: x9dp_dpcd_rd(d, DP_LANE0_1_STATUS, st, 3);
		ret = ret ?: x9dp_dpcd_rd(d, DP_ADJUST_REQUEST_LANE0_1, adj, 2);
		if (ret)
			break;
		dev_info(d->dev, "train %02x EQ: status %02x %02x align %02x, adjust %02x %02x\n",
			 rate, st[0], st[1], st[2], adj[0], adj[1]);
		for (ok = st[2] & DP_INTERLANE_ALIGN_DONE, i = 0; i < lanes; i++) {
			u8 l = st[i / 2] >> (4 * (i & 1));

			if (!(l & DP_LANE_CR_DONE))
				return -EAGAIN;	/* lost CR: a lower rate */
			ok &= (l & (DP_LANE_CHANNEL_EQ_DONE | DP_LANE_SYMBOL_LOCKED)) ==
			      (DP_LANE_CHANNEL_EQ_DONE | DP_LANE_SYMBOL_LOCKED);
		}
		if (ok)
			break;
		for (i = 0; i < lanes; i++) {
			u8 a = adj[i / 2] >> (4 * (i & 1));

			swing[i] = a & 3;
			pre[i] = (a >> 2) & 3;
		}
	}
	if (ret || !ok)
		return ret ?: -EAGAIN;

	/* normal data, scrambling on (displayport_drv.c:556-560) */
	x9dp_pattern(d, 0);
	v[0] = DP_TRAINING_PATTERN_DISABLE;
	ret = x9dp_dpcd_wr(d, DP_TRAINING_PATTERN_SET, v, 1);
	if (!ret)
		dev_info(d->dev, "train: done at %02x x%u, levels %u/%u %u/%u\n", rate, lanes,
			 swing[0], pre[0], swing[1], pre[1]);
	return ret;
}

static int x9dp_train(struct x9dp *d)
{
	u8 cap[DP_RECEIVER_CAP_SIZE], v = DP_SET_POWER_D0, rate, lanes;
	u32 rd_us;
	int ret;

	if (!d->ctrl_on)
		return -EAGAIN;
	ret = x9dp_dpcd_rd(d, DP_DPCD_REV, cap, sizeof(cap));
	if (ret)
		return ret;
	x9dp_dpcd_wr(d, DP_SET_POWER, &v, 1);	/* sink to D0 (displayport_drv.c:2682-2696) */
	rate = min_t(u8, link_rate, cap[DP_MAX_LINK_RATE]);
	lanes = min_t(u8, x9dp_lanes(d), cap[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK);
	/* TRAINING_AUX_RD_INTERVAL: 0 = 400 us, else n * 4 ms (stock :392) */
	rd_us = (cap[DP_TRAINING_AUX_RD_INTERVAL] & DP_TRAINING_AUX_RD_MASK) * 4000 + 400;
	d->rate = 0;
	for (;;) {
		ret = x9dp_train_at(d, rate, lanes, cap[DP_MAX_LANE_COUNT] & DP_ENHANCED_FRAME_CAP,
				    cap[DP_MAX_LANE_COUNT] & DP_TPS3_SUPPORTED, rd_us);
		if (ret != -EAGAIN)
			break;
		dev_info(d->dev, "train: failed at %02x, lower rate\n", rate);
		if (rate == DP_LINK_BW_5_4)
			rate = DP_LINK_BW_2_7;
		else if (rate == DP_LINK_BW_2_7)
			rate = DP_LINK_BW_1_62;
		else
			break;
	}
	if (ret) {
		u8 off = DP_TRAINING_PATTERN_DISABLE;

		x9dp_pattern(d, 0);
		x9dp_dpcd_wr(d, DP_TRAINING_PATTERN_SET, &off, 1);
		return ret;
	}
	d->rate = rate;
	scnprintf(d->last, sizeof(d->last), "train: done, rate %02x (%u Mbit/s per lane) x%u",
		  rate, x9dp_ls_khz(rate) / 100, lanes);
	return 0;
}

/* ---- M3: video, the link's own pattern generator ------------------------------------- */

/*
 * 1080p60, CEA-861 VIC 16 (stock supported_videos V1920X1080P60, v4l2
 * V4L2_DV_BT_CEA_1920X1080P60): 148.5 MHz, h 1920+88+44+148, v 1080+4+5+36,
 * positive syncs. A test mode; the mode list comes from the EDID with DECON2.
 */
static const struct {
	u32 khz, ha, hfp, hs, hbp, va, vfp, vs, vbp;
} x9dp_1080p60 = { 148500, 1920, 88, 44, 148, 1080, 4, 5, 36 };

/*
 * displayport_reg_set_video_configuration + set_bist_video_configuration +
 * displayport_reg_start (displayport_reg.c:1348-1369, :570-729, :1487-1490):
 * VESA range, 8 bpc, RGB, the timing, Mvid/Nvid in master mode, the
 * transfer unit's active symbols, the link as timing master, BIST colour
 * bars, VIDEO_EN.
 */
static int x9dp_video_cfg(struct x9dp *d, u32 *ipp, u32 *fpp)
{
	const typeof(x9dp_1080p60) *m = &x9dp_1080p60;
	u32 ls = x9dp_ls_khz(d->rate), bpp = 24, ip, fp, thr;
	u64 tu;

	if (!d->rate)
		return -EAGAIN;
	/* the SST1 video function, as stock's function_enable does at init (:432-437) */
	link_rmw(d, SYSTEM_SST1_FUNCTION_ENABLE, SST1_VIDEO_FUNC_EN, SST1_VIDEO_FUNC_EN);
	link_rmw(d, SST1_VIDEO_CONTROL, DYNAMIC_RANGE_MODE | VIDEO_BPC | COLOR_FORMAT,
		 FIELD_PREP(VIDEO_BPC, 1));
	writel(m->va + m->vfp + m->vs + m->vbp, d->link + SST1_VIDEO_VERTICAL_TOTAL_PIXELS);
	writel(m->ha + m->hfp + m->hs + m->hbp, d->link + SST1_VIDEO_HORIZONTAL_TOTAL_PIXELS);
	writel(m->va, d->link + SST1_VIDEO_VERTICAL_ACTIVE);
	writel(m->vfp, d->link + SST1_VIDEO_VERTICAL_FRONT_PORCH);
	writel(m->vbp, d->link + SST1_VIDEO_VERTICAL_BACK_PORCH);
	writel(m->ha, d->link + SST1_VIDEO_HORIZONTAL_ACTIVE);
	writel(m->hfp, d->link + SST1_VIDEO_HORIZONTAL_FRONT_PORCH);
	writel(m->hbp, d->link + SST1_VIDEO_HORIZONTAL_BACK_PORCH);
	link_rmw(d, SST1_VIDEO_CONTROL, VSYNC_POLARITY | HSYNC_POLARITY,
		 VSYNC_POLARITY | HSYNC_POLARITY);	/* stock writes SYNC_POSITIVE */

	/* Mvid/Nvid master mode, kHz (displayport_reg.c:636-656) */
	writel(m->khz >> 1, d->link + SST1_MVID_MASTER_MODE);
	writel(ls, d->link + SST1_NVID_MASTER_MODE);
	link_rmw(d, SST1_MAIN_CONTROL, MVID_MODE, MVID_MODE);
	writel(m->khz, d->link + SST1_MVID_SFR_CONFIGURE);
	writel(ls, d->link + SST1_NVID_SFR_CONFIGURE);

	/* transfer unit: active symbols per 32-symbol TU, x1e10 (:658-729) */
	link_rmw(d, SST1_ACTIVE_SYMBOL_MODE_CONTROL, BIT(0), BIT(0));
	link_rmw(d, SST1_ACTIVE_SYMBOL_THRESHOLD_SEL_FEC_OFF, BIT(0), BIT(0));
	link_rmw(d, SST1_ACTIVE_SYMBOL_THRESHOLD_SEL_FEC_ON, BIT(0), BIT(0));
	tu = div64_u64((u64)m->khz * bpp * 32 * 10000000000ULL, (u64)d->nlanes * ls * 8);
	ip = div64_u64(tu, 10000000000ULL);
	fp = div64_u64(tu - (u64)ip * 10000000000ULL, 10);
	thr = ip <= 2 ? 7 : ip <= 5 ? 8 : 9;
	link_rmw(d, SST1_ACTIVE_SYMBOL_INTEGER_FEC_OFF, GENMASK(5, 0), ip);
	link_rmw(d, SST1_ACTIVE_SYMBOL_FRACTION_FEC_OFF, GENMASK(29, 0), fp);
	link_rmw(d, SST1_ACTIVE_SYMBOL_THRESHOLD_FEC_OFF, GENMASK(3, 0), thr);
	tu = div64_u64(tu * 1000, 976);
	ip = div64_u64(tu, 10000000000ULL);
	fp = div64_u64(tu - (u64)ip * 10000000000ULL, 10);
	link_rmw(d, SST1_ACTIVE_SYMBOL_INTEGER_FEC_ON, GENMASK(5, 0), ip);
	link_rmw(d, SST1_ACTIVE_SYMBOL_FRACTION_FEC_ON, GENMASK(29, 0), fp);
	link_rmw(d, SST1_ACTIVE_SYMBOL_THRESHOLD_FEC_ON, GENMASK(3, 0),
		 ip <= 2 ? 7 : ip <= 5 ? 8 : 9);

	link_rmw(d, SST1_VIDEO_MASTER_TIMING_GEN, BIT(0), BIT(0));
	link_rmw(d, SST1_MAIN_CONTROL, VIDEO_MODE, 0);
	*ipp = ip;
	*fpp = fp;
	return 0;
}

static int x9dp_bist(struct x9dp *d)
{
	u32 ip, fp;
	int ret = x9dp_video_cfg(d, &ip, &fp);

	if (ret)
		return ret;
	/* BIST colour bars, stream valid forced (:554-560) */
	link_rmw(d, SST1_VIDEO_BIST_CONTROL, BIST_TYPE, 0);
	link_rmw(d, SST1_VIDEO_CONTROL, STRM_VALID_FORCE | STRM_VALID_CTRL,
		 STRM_VALID_FORCE | STRM_VALID_CTRL);
	link_rmw(d, SST1_VIDEO_BIST_CONTROL, BIST_EN, BIST_EN);
	link_rmw(d, SST1_VIDEO_ENABLE, VIDEO_EN, VIDEO_EN);
	d->video_on = true;
	d->vsrc_decon = false;
	scnprintf(d->last, sizeof(d->last), "bist: 1080p60 colour bars on, TU %u.%09u, Mvid %u Nvid %u",
		  ip, fp, x9dp_1080p60.khz >> 1, x9dp_ls_khz(d->rate));
	return 0;
}

/*
 * The same video configuration with the pixels from DECON2 instead of the
 * pattern generator: displayport_enable (displayport_drv.c:2352-2403) is
 * set_video_configuration + displayport_reg_start, no BIST and no forced
 * stream valid. Stock runs it (the DP subdev's s_stream) before
 * decon_reg_init and decon_reg_start (decon_core.c:631-648, :1848).
 */
static int x9dp_video(struct x9dp *d)
{
	u32 ip, fp;
	int ret;

	if (d->video_on)
		return -EBUSY;		/* video_off first */
	ret = x9dp_video_cfg(d, &ip, &fp);
	if (ret)
		return ret;
	link_rmw(d, SST1_VIDEO_BIST_CONTROL, BIST_EN, 0);
	link_rmw(d, SST1_VIDEO_CONTROL, STRM_VALID_FORCE | STRM_VALID_CTRL, 0);
	link_rmw(d, SST1_VIDEO_ENABLE, VIDEO_EN, VIDEO_EN);
	d->video_on = true;
	d->vsrc_decon = true;
	scnprintf(d->last, sizeof(d->last),
		  "video: 1080p60 from DECON2 on, TU %u.%09u; video_ctl %08x bist %08x video_en %08x",
		  ip, fp, readl(d->link + SST1_VIDEO_CONTROL),
		  readl(d->link + SST1_VIDEO_BIST_CONTROL), readl(d->link + SST1_VIDEO_ENABLE));
	return 0;
}

static int x9dp_video_off(struct x9dp *d)
{
	if (d->ctrl_on) {
		link_rmw(d, SST1_VIDEO_ENABLE, VIDEO_EN, 0);	/* displayport_reg_stop, :1499-1502 */
		link_rmw(d, SST1_VIDEO_BIST_CONTROL, BIST_EN, 0);
		link_rmw(d, SST1_VIDEO_CONTROL, STRM_VALID_FORCE | STRM_VALID_CTRL, 0);
		link_rmw(d, SYSTEM_SST1_FUNCTION_ENABLE, SST1_VIDEO_FUNC_EN, 0);
	}
	d->video_on = false;
	d->vsrc_decon = false;
	scnprintf(d->last, sizeof(d->last), "video_off");
	return 0;
}

/*
 * Read only. The sink: SINK_COUNT, IRQ vector, lane 0-3 status, alignment,
 * SINK_STATUS (0x205: receive port in sync), adjust requests (0x200-0x207),
 * symbol error counts (0x210-0x217). The controller: SST1 function enable
 * (LH power state), SST1 interrupt status (FIFO underflow, VSYNC seen), the
 * video FSM and MAPI debug, the LH P-channel debug, video/BIST/PCS controls.
 */
static int x9dp_status(struct x9dp *d)
{
	u8 a[8] = {}, e[8] = {};
	int r1, r2;

	if (!d->ctrl_on)
		return -EAGAIN;
	r1 = x9dp_dpcd_rd(d, DP_SINK_COUNT, a, sizeof(a));
	r2 = x9dp_dpcd_rd(d, DPCD_SYMBOL_ERROR_COUNT_LANE0, e, sizeof(e));
	scnprintf(d->vstat, sizeof(d->vstat),
		  "sink 200-207: %8ph (%d), errors 210-217: %8ph (%d); sst1_func %08x int0 %08x int1 %08x fsm %08x mapi %08x lh_pch %08x video_en %08x video_ctl %08x bist %08x pcs %08x",
		  a, r1, e, r2, readl(d->link + SYSTEM_SST1_FUNCTION_ENABLE),
		  readl(d->link + SST1_INTERRUPT_STATUS_SET0),
		  readl(d->link + SST1_INTERRUPT_STATUS_SET1),
		  readl(d->link + SST1_VIDEO_DEBUG_FSM_STATE),
		  readl(d->link + SST1_VIDEO_DEBUG_MAPI),
		  readl(d->link + SYSTEM_DEBUG_LH_PCH),
		  readl(d->link + SST1_VIDEO_ENABLE),
		  readl(d->link + SST1_VIDEO_CONTROL),
		  readl(d->link + SST1_VIDEO_BIST_CONTROL),
		  readl(d->link + PCS_CONTROL));
	dev_info(d->dev, "status: %s\n", d->vstat);
	scnprintf(d->last, sizeof(d->last), "status: read (see vstat)");
	return 0;
}

/* SST1_LH_PWR_ON and its status, as displayport_reg_lh_p_ch_power(1) (:1240-1258) */
static int x9dp_lh(struct x9dp *d)
{
	u32 v;
	int ret;

	if (!d->ctrl_on)
		return -EAGAIN;
	link_rmw(d, SYSTEM_SST1_FUNCTION_ENABLE, SST1_LH_PWR_ON, SST1_LH_PWR_ON);
	ret = readl_poll_timeout(d->link + SYSTEM_SST1_FUNCTION_ENABLE, v,
				 v & SST1_LH_PWR_ON_STATUS, 1, 1000);
	scnprintf(d->last, sizeof(d->last), "lh: SST1 LH power %s (%08x, lh_pch %08x)",
		  ret ? "NOT acknowledged" : "on", v, readl(d->link + SYSTEM_DEBUG_LH_PCH));
	return ret;
}

/* ---- M4: read-only look at DECON2 and DMA/DPP channel 1 ------------------------------ */

/*
 * CMU_DPU (vendor cmucal-sfr.c): the DECON2 APB clock gate
 * (GOUT_BLK_DPU_UID_AD_APB_DECON2_IPCLKPORT_PCLKS, +0x201c: CG_VAL b21,
 * auto gating b28) and the Q-channels of the DPU (+0x3020), DPU_DMA
 * (+0x3024) and DPP (+0x3028): ENABLE b0 (clock on request) or CLOCK_REQ b1.
 */
#define CMU_DPU_GATE_APB_DECON2		0x201c
#define CMU_DPU_QCH_DPU			0x3020
#define CMU_DPU_QCH_DPU_DMA		0x3024
#define CMU_DPU_QCH_DPU_DPP		0x3028

static bool x9dp_dpu_ok(struct x9dp *d)
{
	static const u32 qch[] = { CMU_DPU_QCH_DPU, CMU_DPU_QCH_DPU_DMA, CMU_DPU_QCH_DPU_DPP };
	u32 v = 0, i;

	if (regmap_read(d->pmu, d->pd_off, &v) || (v & d->pd_mask) != d->pd_mask) {
		dev_warn(d->dev, "DPU power domain off (PMU %#x = %08x)\n", d->pd_off, v);
		return false;
	}
	v = readl(d->dpu_cmu + CMU_DPU_GATE_APB_DECON2);
	if (!(v & (BIT(21) | BIT(28)))) {
		dev_warn(d->dev, "DECON2 APB clock gated (%08x)\n", v);
		return false;
	}
	for (i = 0; i < ARRAY_SIZE(qch); i++) {
		v = readl(d->dpu_cmu + qch[i]);
		if (!(v & (BIT(0) | BIT(1)))) {
			dev_warn(d->dev, "DPU Q-channel %#x off (%08x)\n", qch[i], v);
			return false;
		}
	}
	return true;
}

static size_t x9dp_m4_range(struct x9dp *d, size_t n, const char *name, void __iomem *base,
			    u32 from, u32 to)
{
	u32 off;

	n += scnprintf(d->m4buf + n, PAGE_SIZE * 4 - n, "%s:\n", name);
	for (off = from; off < to && n < PAGE_SIZE * 4 - 64; off += 16)
		n += scnprintf(d->m4buf + n, PAGE_SIZE * 4 - n, "  %04x: %08x %08x %08x %08x\n",
			       off, readl(base + off), readl(base + off + 4),
			       readl(base + off + 8), readl(base + off + 12));
	return n;
}

static int x9dp_m4dump(struct x9dp *d)
{
	size_t n = 0;

	if (!d->decon2 || !d->dma1 || !d->dpp1 || !d->dma_com || !d->dpu_cmu)
		return -ENODEV;		/* the DT does not describe them */
	if (!x9dp_dpu_ok(d))
		return -EIO;
	if (!d->m4buf) {
		d->m4buf = devm_kzalloc(d->dev, PAGE_SIZE * 4, GFP_KERNEL);
		if (!d->m4buf)
			return -ENOMEM;
	}
	n += scnprintf(d->m4buf + n, PAGE_SIZE * 4 - n,
		       "cmu_dpu: apb_decon2 %08x qch dpu %08x dma %08x dpp %08x\n",
		       readl(d->dpu_cmu + CMU_DPU_GATE_APB_DECON2),
		       readl(d->dpu_cmu + CMU_DPU_QCH_DPU),
		       readl(d->dpu_cmu + CMU_DPU_QCH_DPU_DMA),
		       readl(d->dpu_cmu + CMU_DPU_QCH_DPU_DPP));
	n = x9dp_m4_range(d, n, "DECON2", d->decon2, 0, min_t(u32, 0x400, d->decon2_sz));
	n = x9dp_m4_range(d, n, "DMA ch1", d->dma1, 0, min_t(u32, 0x70, d->dma1_sz));
	if (d->dma1_sz >= 0x880)
		n = x9dp_m4_range(d, n, "DMA ch1 err", d->dma1, 0x870, 0x880);
	n = x9dp_m4_range(d, n, "DPP ch1", d->dpp1, 0, min_t(u32, 0x50, d->dpp1_sz));
	n = x9dp_m4_range(d, n, "DMA common", d->dma_com, 0, min_t(u32, 0x110, d->dma_com_sz));
	d->m4len = n;
	scnprintf(d->last, sizeof(d->last), "m4dump: %zu bytes, see m4regs", n);
	return 0;
}

/* ---- M4: the approved writes, one debugfs step each --------------------------------- */

/*
 * Stock DECON2 bring-up for DP (decon_reg.c, regs-decon.h, dpp_reg.c,
 * regs-dpp.h; dpu_9810, tw10-android-4.9-q). Only these registers are
 * written:
 *
 *   W1 route  DECON0 DP_CONNECTION_CONTROL (+0x270) [2:0] := 2, RMW
 *             (decon_reg_set_interface, decon_reg.c:609-611)
 *   W2 route  disp sysreg DISP_DPU_TE_QACTIVE_PLL_EN (+0x10) bit 0
 *             (dpu_reg_set_qactive_pll, decon_reg.c:17-25)
 *   W4 sram   DECON2 SRAM_SHARE_ENABLE (+0x30) := SRAM1|2|3, 12K
 *             (decon_reg_set_sram_share, decon_reg.c:219-259, :1815-1816)
 *   W5 lh     the SST1 LH power handshake (x9dp_lh), and its release in
 *             decon2_stop (decon_reg_stop_inst, decon_reg.c:1937-1964)
 *   W6 dma    DPU_DMA channel 1 (IDMA_G1) and DPP channel 1, per-channel
 *             registers only: nothing in the DMA common block
 *   DECON2's own block: decon2, decon2_start, decon2_stop
 *
 * Not written: DECON0's CLOCK_CONTROL_0 TE_QACTIVE_PLL_ON (W3,
 * decon_reg.c:198-206), DECON0's blender windows (0x1000 + 0x30 * w,
 * decon_reg_set_window_control, decon_reg.c:2221-2242), the DMA common
 * QoS LUT (already 0x44444444). Without a window DECON2's blender sends
 * its background colour, which is the visible test here.
 */
#define DECON_GLOBAL_CONTROL		0x0000
#define  DECON_SRESET			BIT(28)
#define  DECON_TEN_BPC			BIT(20)
#define  DECON_OPERATION_MODE_CMD	BIT(8)
#define  DECON_RUN_STATUS		BIT(4)
#define  DECON_EN			BIT(1)
#define  DECON_EN_F			BIT(0)
#define DECON_SRAM_SHARE_ENABLE		0x0030
#define  DECON2_SRAM_12K		(BIT(16) | BIT(20) | BIT(24))	/* SRAM1-3 */
#define DECON_INTERRUPT_PENDING		0x004c
#define DECON_EXTRA_INTERRUPT_PENDING	0x0050
#define DECON_SHADOW_REG_UPDATE_REQ	0x0060
#define  DECON_SHADOW_GLOBAL		BIT(31)
#define DECON_HW_SW_TRIG_CONTROL	0x0070
#define  DECON_HW_TRIG_EN		BIT(0)
#define DECON_SCALED_SIZE_CONTROL_0	0x00a0
#define DECON_CLOCK_CONTROL_0		0x00f0
#define  DECON_CLOCK_CG_QACTIVE		(BIT(0) | BIT(8) | BIT(12) | BIT(16) | BIT(24) | BIT(28))
#define DECON_OUTFIFO_SIZE_CONTROL_0	0x0120
#define DECON_OUTFIFO_TH_CONTROL_0	0x012c
#define  DECON_OUTFIFO_TH_1H		5
#define DECON_OUTFIFO_DATA_ORDER	0x0130
#define  DECON_PIXEL_ORDER_SWAP		GENMASK(6, 4)	/* 0 RGB */
#define DECON_BLENDER_BG_IMAGE_SIZE_0	0x0200
#define DECON_BLENDER_BG_COLOR_0	0x0208		/* A [23:16], R [9:0] */
#define DECON_BLENDER_BG_COLOR_1	0x020c		/* G [25:16], B [9:0] */
#define DECON_DATA_PATH_CONTROL_0	0x0214
#define DECON_DATA_PATH_CONTROL_2	0x0230
#define  DECON_SCALER_PATH		GENMASK(25, 24)
#define  DECON_COMP_OUTIF_PATH		GENMASK(7, 0)
#define  DECON2_NOCOMP_OUTFIFO0_DPIF	0x008		/* decon.h:362 */
#define DECON_FRAME_COUNT		0x02a0
#define DECON_SHADOW			0x7000		/* SHD_DECON */
#define DECON_DP_CONN_SEL_DP0		GENMASK(2, 0)
#define DISP_TE_QACTIVE_PLL_EN		BIT(0)

#define IDMA_ENABLE			0x0000
#define  IDMA_SRAM_CLOCK_GATE_EN	BIT(9)
#define IDMA_IN_CON			0x0008
#define  IDMA_IMG_FORMAT		GENMASK(15, 11)
#define  IDMA_FMT_XRGB8888		4
#define  IDMA_ROTATION			GENMASK(10, 8)
#define  IDMA_AFBC_EN			BIT(7)
#define  IDMA_BLOCK_EN			BIT(3)
#define IDMA_OUT_CON			0x000c
#define  IDMA_OUT_FRAME_ALPHA		GENMASK(31, 24)
#define IDMA_SRC_SIZE			0x0010
#define IDMA_SRC_OFFSET			0x0014
#define IDMA_IMG_SIZE			0x0018
#define IDMA_IN_BASE_ADDR_Y		0x0040
#define IDMA_IN_BASE_ADDR_C		0x0044
#define IDMA_CFG_ERR_STATE		0x0870
#define DPP_IN_CON			0x0008
#define  DPP_ALPHA_SEL			BIT(3)
#define  DPP_IMG_FORMAT			GENMASK(2, 0)	/* 0 ARGB8888 */
#define DPP_IMG_SIZE			0x0018
#define DPP_MAIN_H_RATIO		0x0044
#define DPP_MAIN_V_RATIO		0x0048
#define  DPP_RATIO_1			(1 << 20)

static uint bg_rgb = 0xff8000;
module_param(bg_rgb, uint, 0644);
MODULE_PARM_DESC(bg_rgb, "DECON2 blender background for the decon2 step, 0xRRGGBB (default 0xff8000 orange; red and blue swapped would show azure)");

/* the M4 ranges the step needs, and the DPU powered and clocked */
static int x9dp_m4_ok(struct x9dp *d, bool route)
{
	if (!d->decon2 || !d->dma1 || !d->dpp1 || !d->dpu_cmu ||
	    (route && (!d->d0conn || !d->dsys)))
		return -ENODEV;
	return x9dp_dpu_ok(d) ? 0 : -EIO;
}

static void x9dp_d2_rmw(struct x9dp *d, u32 reg, u32 mask, u32 val)
{
	u32 v = readl(d->decon2 + reg);

	writel((v & ~mask) | (val & mask), d->decon2 + reg);
}

/* W1 + W2: DECON2's output to the DP link, the TE/QACTIVE PLL enable */
static int x9dp_route(struct x9dp *d)
{
	u32 c0, c1, s0, s1;
	int ret = x9dp_m4_ok(d, true);

	if (ret)
		return ret;
	c0 = readl(d->d0conn);
	writel((c0 & ~DECON_DP_CONN_SEL_DP0) | 2, d->d0conn);
	c1 = readl(d->d0conn);
	s0 = readl(d->dsys);
	writel(s0 | DISP_TE_QACTIVE_PLL_EN, d->dsys);
	s1 = readl(d->dsys);
	scnprintf(d->last, sizeof(d->last),
		  "route: DP_CONNECTION %08x -> %08x (DECON0 shadow %08x), TE_QACTIVE_PLL_EN %08x -> %08x",
		  c0, c1, d->d0conn_shd ? readl(d->d0conn_shd) : 0xdeadbeef, s0, s1);
	return (c1 & DECON_DP_CONN_SEL_DP0) == 2 && (s1 & DISP_TE_QACTIVE_PLL_EN) ? 0 : -EIO;
}

/* W4: DECON2's 12K of line SRAM (SRAM1-3; DECON0 keeps SRAM0) */
static int x9dp_sram(struct x9dp *d)
{
	u32 v;
	int ret = x9dp_m4_ok(d, false);

	if (ret)
		return ret;
	if (readl(d->decon2 + DECON_GLOBAL_CONTROL) & DECON_RUN_STATUS)
		return -EBUSY;
	writel(DECON2_SRAM_12K, d->decon2 + DECON_SRAM_SHARE_ENABLE);
	v = readl(d->decon2 + DECON_SRAM_SHARE_ENABLE);
	scnprintf(d->last, sizeof(d->last), "sram: SRAM_SHARE_ENABLE %08x (shadow %08x)", v,
		  readl(d->decon2 + DECON_SHADOW + DECON_SRAM_SHARE_ENABLE));
	return v == DECON2_SRAM_12K ? 0 : -EIO;
}

static u32 x9dp_c10(u32 c8)
{
	return (c8 << 2) | (c8 >> 6);	/* 8 to 10 bits, 0xff -> 0x3ff */
}

/*
 * DECON2's own block, decon_reg_init for id 2 out_type DP (decon_reg.c:
 * 1789-1842) without the writes outside it: clock gating and QACTIVE
 * off (:188-196), video mode (:73-83), blender and scaled size 1920x1080
 * (:347-361, :261-270), HW trigger (:754-766), RGB order (DP, :1686-1691),
 * data path NOCOMP_OUTFIFO0_DPIF (:397-405), OUTFIFO size and 1H threshold
 * (:298-312), 8 bpc (:640-650), per-frame off (:94-97), interrupts
 * cleared (:2023-2034). Then the blender background colour (bg_rgb) and
 * no window enabled on DECON2.
 */
static int x9dp_decon2(struct x9dp *d)
{
	const u32 w = x9dp_1080p60.ha, h = x9dp_1080p60.va;
	u32 r = (bg_rgb >> 16) & 0xff, g = (bg_rgb >> 8) & 0xff, b = bg_rgb & 0xff;
	int ret = x9dp_m4_ok(d, false);

	if (ret)
		return ret;
	if (readl(d->decon2 + DECON_GLOBAL_CONTROL) & DECON_RUN_STATUS)
		return -EBUSY;		/* decon2_stop first */
	x9dp_d2_rmw(d, DECON_CLOCK_CONTROL_0, DECON_CLOCK_CG_QACTIVE, 0);
	x9dp_d2_rmw(d, DECON_GLOBAL_CONTROL, DECON_OPERATION_MODE_CMD, 0);
	x9dp_d2_rmw(d, DECON_BLENDER_BG_IMAGE_SIZE_0, GENMASK(29, 16) | GENMASK(13, 0),
		    h << 16 | w);
	x9dp_d2_rmw(d, DECON_SCALED_SIZE_CONTROL_0, GENMASK(29, 16) | GENMASK(13, 0), h << 16 | w);
	x9dp_d2_rmw(d, DECON_HW_SW_TRIG_CONTROL, DECON_HW_TRIG_EN, DECON_HW_TRIG_EN);
	x9dp_d2_rmw(d, DECON_OUTFIFO_DATA_ORDER, DECON_PIXEL_ORDER_SWAP, 0);
	x9dp_d2_rmw(d, DECON_DATA_PATH_CONTROL_2, DECON_SCALER_PATH | DECON_COMP_OUTIF_PATH,
		    DECON2_NOCOMP_OUTFIFO0_DPIF);
	writel(h << 16 | w, d->decon2 + DECON_OUTFIFO_SIZE_CONTROL_0);
	x9dp_d2_rmw(d, DECON_OUTFIFO_TH_CONTROL_0, GENMASK(2, 0), DECON_OUTFIFO_TH_1H);
	x9dp_d2_rmw(d, DECON_GLOBAL_CONTROL, DECON_TEN_BPC | DECON_EN_F, 0);
	x9dp_d2_rmw(d, DECON_INTERRUPT_PENDING, BIT(12) | BIT(13), ~0);
	x9dp_d2_rmw(d, DECON_EXTRA_INTERRUPT_PENDING, BIT(4) | BIT(8), ~0);
	writel(0xff << 16 | x9dp_c10(r), d->decon2 + DECON_BLENDER_BG_COLOR_0);
	writel(x9dp_c10(g) << 16 | x9dp_c10(b), d->decon2 + DECON_BLENDER_BG_COLOR_1);
	scnprintf(d->last, sizeof(d->last),
		  "decon2: global %08x clk %08x trig %08x bg %08x/%08x size %08x path %08x/%08x outfifo %08x th %08x order %08x",
		  readl(d->decon2 + DECON_GLOBAL_CONTROL), readl(d->decon2 + DECON_CLOCK_CONTROL_0),
		  readl(d->decon2 + DECON_HW_SW_TRIG_CONTROL),
		  readl(d->decon2 + DECON_BLENDER_BG_COLOR_0),
		  readl(d->decon2 + DECON_BLENDER_BG_COLOR_1),
		  readl(d->decon2 + DECON_BLENDER_BG_IMAGE_SIZE_0),
		  readl(d->decon2 + DECON_DATA_PATH_CONTROL_0),
		  readl(d->decon2 + DECON_DATA_PATH_CONTROL_2),
		  readl(d->decon2 + DECON_OUTFIFO_SIZE_CONTROL_0),
		  readl(d->decon2 + DECON_OUTFIFO_TH_CONTROL_0),
		  readl(d->decon2 + DECON_OUTFIFO_DATA_ORDER));
	return 0;
}

/*
 * decon_reg_start for DP (decon_reg.c:1844-1864): the LH channel is up
 * (the lh step, W5, which stock does first in this function), DECON_EN
 * and DECON_EN_F, the global shadow update, RUN_STATUS within 20 ms.
 */
static int x9dp_decon2_start(struct x9dp *d)
{
	u32 v;
	int ret = x9dp_m4_ok(d, true);

	if (ret)
		return ret;
	if (!d->ctrl_on ||
	    !(readl(d->link + SYSTEM_SST1_FUNCTION_ENABLE) & SST1_LH_PWR_ON_STATUS))
		return -EAGAIN;		/* lh first */
	if ((readl(d->d0conn) & DECON_DP_CONN_SEL_DP0) != 2 ||
	    readl(d->decon2 + DECON_SRAM_SHARE_ENABLE) != DECON2_SRAM_12K)
		return -EAGAIN;		/* route and sram first */
	x9dp_d2_rmw(d, DECON_GLOBAL_CONTROL, DECON_EN | DECON_EN_F, ~0);
	x9dp_d2_rmw(d, DECON_SHADOW_REG_UPDATE_REQ, DECON_SHADOW_GLOBAL, ~0);
	ret = readl_poll_timeout(d->decon2 + DECON_GLOBAL_CONTROL, v, v & DECON_RUN_STATUS,
				 10, 20000);
	d->decon2_on = true;		/* decon2_stop undoes it either way */
	scnprintf(d->last, sizeof(d->last),
		  "decon2_start: %s; global %08x (shadow %08x) shadow_req %08x int %08x extra %08x frames %08x",
		  ret ? "NOT running" : "running", v,
		  readl(d->decon2 + DECON_SHADOW + DECON_GLOBAL_CONTROL),
		  readl(d->decon2 + DECON_SHADOW_REG_UPDATE_REQ),
		  readl(d->decon2 + DECON_INTERRUPT_PENDING),
		  readl(d->decon2 + DECON_EXTRA_INTERRUPT_PENDING),
		  readl(d->decon2 + DECON_FRAME_COUNT));
	return ret;
}

/*
 * decon_reg_stop for DP: instant stop (decon_reg.c:1937-1964), the LH
 * channel released, RUN_STATUS off, then the soft reset (:50-67), all in
 * DECON2's block and the link's.
 */
static int x9dp_decon2_stop(struct x9dp *d)
{
	u32 v;
	int ret;

	if (!d->decon2 || !x9dp_dpu_ok(d))
		return -EIO;
	x9dp_d2_rmw(d, DECON_GLOBAL_CONTROL, DECON_EN | DECON_EN_F, 0);
	x9dp_d2_rmw(d, DECON_SHADOW_REG_UPDATE_REQ, DECON_SHADOW_GLOBAL, ~0);
	if (d->ctrl_on)
		link_rmw(d, SYSTEM_SST1_FUNCTION_ENABLE, SST1_LH_PWR_ON, 0);
	ret = readl_poll_timeout(d->decon2 + DECON_GLOBAL_CONTROL, v, !(v & DECON_RUN_STATUS),
				 10, 25000);
	if (!ret) {
		x9dp_d2_rmw(d, DECON_GLOBAL_CONTROL, DECON_SRESET, ~0);
		ret = readl_poll_timeout(d->decon2 + DECON_GLOBAL_CONTROL, v, !(v & DECON_SRESET),
					 10, 20000);
	}
	d->decon2_on = false;
	scnprintf(d->last, sizeof(d->last), "decon2_stop: %s, global %08x, sst1_func %08x",
		  ret ? "timed out" : "stopped and reset", v,
		  d->ctrl_on ? readl(d->link + SYSTEM_SST1_FUNCTION_ENABLE) : 0);
	return ret;
}

/*
 * The test image: red rises left to right, green top to bottom, blue
 * constant at 0x40, a white 16-pixel frame and a white diagonal; nothing
 * like the BIST bars.
 */
static void x9dp_fb_fill(u32 *fb, u32 w, u32 h)
{
	u32 x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			u32 dy = x * h / w;
			bool white = x < 16 || y < 16 || x >= w - 16 || y >= h - 16 ||
				     (dy >= y && dy < y + 4);

			fb[y * w + x] = white ? 0x00ffffff :
					(x * 255 / (w - 1)) << 16 | (y * 255 / (h - 1)) << 8 | 0x40;
		}
}

/*
 * W6: DMA/DPP channel 1 (IDMA_G1, stock idma_g1) reading the test image,
 * 1920x1080 XRGB8888. dpp_reg_init + dpp_reg_configure_params for G1
 * (dpp_reg.c:1310-1328, :1500-1510): DMA SRAM clock gating off (:160-166),
 * frame alpha 0xff (:1208-1211), no rotation, no block, no AFBC, source
 * offset/size, image size, base address (:1167-1184, :1113-1165), DMA
 * XRGB8888 and DPP ARGB8888 without per-pixel alpha (:986-1107), DPP
 * ratio 1:1 (:746-760). G1 has no CSC (:678-679). Interrupts stay masked
 * (no handler), the QoS LUT in the common block is left as it is. The
 * channel fetches only for a DECON window mapped to it (blender channel
 * 3, helper.c:604-633), which this step does not set up.
 */
static int x9dp_dma(struct x9dp *d)
{
	const u32 w = x9dp_1080p60.ha, h = x9dp_1080p60.va;
	u32 size = h << 16 | w;
	int ret = x9dp_m4_ok(d, false);

	if (ret)
		return ret;
	if (!d->fb_cpu) {
		ret = dma_set_mask_and_coherent(d->dev, DMA_BIT_MASK(32));
		if (ret)
			return ret;
		d->fb_sz = (size_t)w * h * 4;
		d->fb_cpu = dma_alloc_wc(d->dev, d->fb_sz, &d->fb_dma, GFP_KERNEL);
		if (!d->fb_cpu)
			return -ENOMEM;
		x9dp_fb_fill(d->fb_cpu, w, h);
		wmb();
	}
	writel(readl(d->dma1 + IDMA_ENABLE) & ~IDMA_SRAM_CLOCK_GATE_EN, d->dma1 + IDMA_ENABLE);
	writel((readl(d->dma1 + IDMA_OUT_CON) & ~IDMA_OUT_FRAME_ALPHA) |
	       FIELD_PREP(IDMA_OUT_FRAME_ALPHA, 0xff), d->dma1 + IDMA_OUT_CON);
	writel(0, d->dma1 + IDMA_SRC_OFFSET);
	writel(size, d->dma1 + IDMA_SRC_SIZE);
	writel(size, d->dma1 + IDMA_IMG_SIZE);
	writel((u32)d->fb_dma, d->dma1 + IDMA_IN_BASE_ADDR_Y);
	writel((u32)d->fb_dma, d->dma1 + IDMA_IN_BASE_ADDR_C);
	writel((readl(d->dma1 + IDMA_IN_CON) &
		~(IDMA_IMG_FORMAT | IDMA_ROTATION | IDMA_AFBC_EN | IDMA_BLOCK_EN)) |
	       FIELD_PREP(IDMA_IMG_FORMAT, IDMA_FMT_XRGB8888), d->dma1 + IDMA_IN_CON);
	writel(size, d->dpp1 + DPP_IMG_SIZE);
	writel(readl(d->dpp1 + DPP_IN_CON) & ~(DPP_ALPHA_SEL | DPP_IMG_FORMAT),
	       d->dpp1 + DPP_IN_CON);
	writel((readl(d->dpp1 + DPP_MAIN_H_RATIO) & ~GENMASK(23, 0)) | DPP_RATIO_1,
	       d->dpp1 + DPP_MAIN_H_RATIO);
	writel((readl(d->dpp1 + DPP_MAIN_V_RATIO) & ~GENMASK(23, 0)) | DPP_RATIO_1,
	       d->dpp1 + DPP_MAIN_V_RATIO);
	scnprintf(d->last, sizeof(d->last),
		  "dma: fb %pad; en %08x in_con %08x out_con %08x src %08x img %08x base %08x err %08x; dpp en %08x in_con %08x img %08x",
		  &d->fb_dma, readl(d->dma1 + IDMA_ENABLE), readl(d->dma1 + IDMA_IN_CON),
		  readl(d->dma1 + IDMA_OUT_CON), readl(d->dma1 + IDMA_SRC_SIZE),
		  readl(d->dma1 + IDMA_IMG_SIZE), readl(d->dma1 + IDMA_IN_BASE_ADDR_Y),
		  d->dma1_sz >= 0x874 ? readl(d->dma1 + IDMA_CFG_ERR_STATE) : 0,
		  readl(d->dpp1), readl(d->dpp1 + DPP_IN_CON), readl(d->dpp1 + DPP_IMG_SIZE));
	return 0;
}

static int x9dp_m4regs_show(struct seq_file *s, void *unused)
{
	struct x9dp *d = s->private;

	mutex_lock(&d->lock);
	if (d->m4buf && d->m4len)
		seq_write(s, d->m4buf, d->m4len);
	else
		seq_puts(s, "no dump yet: echo m4dump > step\n");
	mutex_unlock(&d->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(x9dp_m4regs);

static void __iomem *x9dp_map_opt(struct platform_device *pdev, const char *name, u32 *sz)
{
	struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);

	if (!res)
		return NULL;
	if (sz)
		*sz = resource_size(res);
	return devm_ioremap(&pdev->dev, res->start, resource_size(res));
}

static const struct {
	const char *name;
	int (*fn)(struct x9dp *d);
} phases[] = {
	{ "id", x9dp_id }, { "pma", x9dp_pma }, { "phy", x9dp_phy }, { "ctrl", x9dp_ctrl },
	{ "dpcd", x9dp_dpcd }, { "edid", x9dp_edid }, { "train", x9dp_train },
	{ "bist", x9dp_bist }, { "video_off", x9dp_video_off }, { "status", x9dp_status },
	{ "lh", x9dp_lh }, { "m4dump", x9dp_m4dump }, { "off", x9dp_off },
	{ "route", x9dp_route }, { "sram", x9dp_sram }, { "decon2", x9dp_decon2 },
	{ "video", x9dp_video }, { "decon2_start", x9dp_decon2_start },
	{ "decon2_stop", x9dp_decon2_stop }, { "dma", x9dp_dma },
};

static int x9dp_run(struct x9dp *d, const char *name)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(phases); i++)
		if (!strcmp(name, phases[i].name))
			break;
	if (i == ARRAY_SIZE(phases))
		return -EINVAL;
	ret = phases[i].fn(d);
	if (ret)
		scnprintf(d->last, sizeof(d->last), "%s: failed %d", name, ret);
	dev_info(d->dev, "%s\n", d->last);
	return ret;
}

static void x9dp_auto_work(struct work_struct *work)
{
	struct x9dp *d = container_of(work, struct x9dp, auto_work);
	static const char * const up[] = { "pma", "phy", "ctrl", "dpcd", "edid" };
	int i, n = auto_bringup ? ARRAY_SIZE(up) : 1;	/* pma_early: the PMA only */

	mutex_lock(&d->lock);
	if (x9dp_lanes(d)) {
		for (i = 0; i < n; i++)
			if (x9dp_run(d, up[i]))
				break;
	} else if (d->pma_on) {
		x9dp_run(d, "off");
	}
	mutex_unlock(&d->lock);
}

static void x9dp_hpd_work(struct work_struct *work)
{
	struct x9dp *d = container_of(work, struct x9dp, hpd_work);
	int i;

	mutex_lock(&d->lock);
	if (!(d->dp_status & DP_STATUS_HPD_STATE) || !x9dp_lanes(d))
		goto out;
	if ((!d->id_done && x9dp_run(d, "id")) ||
	    (!d->pma_on && x9dp_run(d, "pma")) ||
	    (!d->phy_on && x9dp_run(d, "phy")) ||
	    (!d->ctrl_on && x9dp_run(d, "ctrl")))
		goto out;
	if (x9dp_run(d, "dpcd") == 0)
		dev_info(d->dev, "DPCD %16ph\n", d->dpcd);
	if (x9dp_run(d, "edid") == 0)
		for (i = 0; i < EDID_LENGTH; i += 16)
			dev_info(d->dev, "EDID %02x: %16ph\n", i, d->edid + i);
	/*
	 * Every HPD rise: the sink lost the link while HPD was low, so the
	 * video stops, the link is trained again and the bars restart (or,
	 * with DECON2 running, its video); the sink's view 100 ms into the
	 * video is logged.
	 */
	if (hpd_bist) {
		if (d->video_on)
			x9dp_run(d, "video_off");
		if (x9dp_run(d, "train") == 0 &&
		    x9dp_run(d, d->decon2_on ? "video" : "bist") == 0) {
			mutex_unlock(&d->lock);
			msleep(100);
			mutex_lock(&d->lock);
			x9dp_run(d, "status");
		}
	}
out:
	mutex_unlock(&d->lock);
}

/* ---- typec: orientation and DP state ---------------------------------------- */

static int x9dp_sw_set(struct typec_switch_dev *sw, enum typec_orientation orientation)
{
	struct x9dp *d = typec_switch_get_drvdata(sw);

	mutex_lock(&d->lock);
	if (orientation != TYPEC_ORIENTATION_NONE)
		d->orient = orientation;
	mutex_unlock(&d->lock);
	return 0;
}

static int x9dp_mux_set(struct typec_mux_dev *mux, struct typec_mux_state *state)
{
	struct x9dp *d = typec_mux_get_drvdata(mux);
	bool was, now, hpd_up = false;

	/*
	 * The USB Safe State is what typec_displayport sets around Enter Mode
	 * and each Configure: a pause between DP states, not their end (USB, or
	 * the orientation going to none, is). Nothing changes for it, so the
	 * PMA stays up through Enter and Configure, as on stock.
	 */
	if (state->mode == TYPEC_STATE_SAFE)
		return 0;

	mutex_lock(&d->lock);
	was = x9dp_lanes(d);
	d->mode = state->mode;
	now = x9dp_lanes(d);
	if (now && state->data) {
		const struct typec_displayport_data *dp = state->data;

		if (dp->status != d->dp_status)
			dev_info(d->dev, "DP status %08x: HPD %s\n", dp->status,
				 dp->status & DP_STATUS_HPD_STATE ? "high" : "low");
		hpd_up = (dp->status & DP_STATUS_HPD_STATE) &&
			 !(d->dp_status & DP_STATUS_HPD_STATE);
		d->dp_status = dp->status;
		d->dp_conf = dp->conf;
	}
	if (was != now)
		dev_info(d->dev, "pin state %lu: %d lanes, %s\n", state->mode, x9dp_lanes(d),
			 d->orient == TYPEC_ORIENTATION_REVERSE ? "reverse" : "normal");
	mutex_unlock(&d->lock);
	if ((auto_bringup || pma_early) && was != now)
		schedule_work(&d->auto_work);
	if (hpd_read && hpd_up)
		schedule_work(&d->hpd_work);
	return 0;
}

/* ---- debugfs ------------------------------------------------------------------ */

static int x9dp_state_show(struct seq_file *s, void *unused)
{
	struct x9dp *d = s->private;

	mutex_lock(&d->lock);
	seq_printf(s, "pin state %lu (%d lanes), orientation %s, DP status %08x (HPD %s), conf %08x\n",
		   d->mode, x9dp_lanes(d),
		   d->orient == TYPEC_ORIENTATION_REVERSE ? "reverse" : "normal", d->dp_status,
		   d->dp_status & DP_STATUS_HPD_STATE ? "high" : "low", d->dp_conf);
	seq_printf(s, "id %d pma %d phy %d ctrl %d; PHY %s; link %s\n", d->id_done, d->pma_on,
		   d->phy_on, d->ctrl_on, x9dp_phy_ok(d) ? "not isolated" : "ISOLATED",
		   x9dp_link_ok(d) ? "powered and clocked" : "OFF");
	seq_printf(s, "link: rate %02x x%u, video %s; DECON2 %s\n", d->rate,
		   d->rate ? d->nlanes : 0,
		   !d->video_on ? "off" : d->vsrc_decon ? "on (DECON2)" : "on (colour bars)",
		   d->decon2_on ? "started" : "stopped");
	seq_printf(s, "last: %s\n", d->last);
	if (d->vstat[0])
		seq_printf(s, "vstat: %s\n", d->vstat);
	if (d->dpcd_ok)
		seq_printf(s, "dpcd: %16phN\n", d->dpcd);
	if (d->edid_ok) {
		int i;

		seq_puts(s, "edid:\n");
		for (i = 0; i < EDID_LENGTH; i += 16)
			seq_printf(s, "%16ph\n", d->edid + i);
	}
	mutex_unlock(&d->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(x9dp_state);

static ssize_t x9dp_step_write(struct file *file, const char __user *ubuf, size_t len,
			       loff_t *ppos)
{
	struct x9dp *d = file->private_data;
	char buf[24];
	int ret;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;
	strim(buf);
	mutex_lock(&d->lock);
	/*
	 * By hand, the bring-up steps would re-program the PHY and the link
	 * under a running video stream: the sink loses the link and drops HPD
	 * (dp12: train under the bars, -EIO, HPD low). Not while video is on;
	 * video_off or off first.
	 */
	if (d->video_on && (!strcmp(buf, "id") || !strcmp(buf, "pma") || !strcmp(buf, "phy") ||
			    !strcmp(buf, "ctrl") || !strcmp(buf, "train"))) {
		dev_info(d->dev, "%s refused: video is on (video_off or off first)\n", buf);
		ret = -EBUSY;
	} else {
		ret = x9dp_run(d, buf);
	}
	mutex_unlock(&d->lock);
	return ret ? ret : len;
}

static const struct file_operations x9dp_step_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = x9dp_step_write,
};

/* ---- probe -------------------------------------------------------------------- */

static int x9dp_read_pairs(struct device *dev, const char *prop, u32 *out, int n)
{
	int ret = of_property_read_u32_array(dev->of_node, prop, out, n);

	if (ret)
		return dev_err_probe(dev, ret, "%s: %d cells needed\n", prop, n);
	return 0;
}

static void x9dp_unregister(void *data)
{
	struct x9dp *d = data;

	cancel_work_sync(&d->auto_work);
	cancel_work_sync(&d->hpd_work);
	debugfs_remove_recursive(d->dbg);
	mutex_lock(&d->lock);
	x9dp_off(d);
	if (d->fb_cpu)
		dma_free_wc(d->dev, d->fb_sz, d->fb_cpu, d->fb_dma);
	mutex_unlock(&d->lock);
	typec_mux_unregister(d->mux);
	typec_switch_unregister(d->sw);
}

static void x9dp_iounmap(void *addr)
{
	iounmap(addr);
}

static int x9dp_probe(struct platform_device *pdev)
{
	struct typec_switch_desc sw_desc = {};
	struct typec_mux_desc mux_desc = {};
	struct device *dev = &pdev->dev;
	struct device_node *usb;
	struct x9dp *d;
	u32 iso[2];
	int ret, i;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->dev = dev;
	mutex_init(&d->lock);
	INIT_WORK(&d->auto_work, x9dp_auto_work);
	INIT_WORK(&d->hpd_work, x9dp_hpd_work);
	d->orient = TYPEC_ORIENTATION_NORMAL;
	d->mode = TYPEC_STATE_SAFE;

	d->link = devm_platform_ioremap_resource_byname(pdev, "link");
	if (IS_ERR(d->link))
		return dev_err_probe(dev, PTR_ERR(d->link), "link registers\n");
	d->phy = devm_platform_ioremap_resource_byname(pdev, "phy");
	if (IS_ERR(d->phy))
		return dev_err_probe(dev, PTR_ERR(d->phy), "combo PHY registers\n");
	/* USBCON belongs to the USB PHY driver: mapped, not requested */
	usb = of_parse_phandle(dev->of_node, "samsung,usb-phy", 0);
	if (!usb)
		return dev_err_probe(dev, -ENODEV, "samsung,usb-phy: no USB PHY node\n");
	d->usbcon = of_iomap(usb, 0);
	of_node_put(usb);
	if (!d->usbcon)
		return dev_err_probe(dev, -ENOMEM, "USBCON registers\n");
	ret = devm_add_action_or_reset(dev, x9dp_iounmap, d->usbcon);
	if (ret)
		return ret;
	d->pmu = syscon_regmap_lookup_by_phandle(dev->of_node, "samsung,pmu-syscon");
	if (IS_ERR(d->pmu))
		return dev_err_probe(dev, PTR_ERR(d->pmu), "samsung,pmu-syscon\n");
	{
		struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "link-qch");

		if (!res)
			return dev_err_probe(dev, -EINVAL, "reg \"link-qch\": the link's clock Q-channels\n");
		/* the CMU's: mapped to read, not requested */
		d->qch = devm_ioremap(dev, res->start, resource_size(res));
		if (!d->qch)
			return -ENOMEM;
		d->qch_n = resource_size(res) / 4;
	}
	/* M4, optional */
	d->decon2 = x9dp_map_opt(pdev, "decon2", &d->decon2_sz);
	d->dma1 = x9dp_map_opt(pdev, "dma1", &d->dma1_sz);
	d->dpp1 = x9dp_map_opt(pdev, "dpp1", &d->dpp1_sz);
	d->dma_com = x9dp_map_opt(pdev, "dma-common", &d->dma_com_sz);
	d->dpu_cmu = x9dp_map_opt(pdev, "dpu-cmu", NULL);
	d->d0conn = x9dp_map_opt(pdev, "decon0-dp-conn", NULL);
	d->d0conn_shd = x9dp_map_opt(pdev, "decon0-dp-conn-shadow", NULL);
	d->dsys = x9dp_map_opt(pdev, "disp-sysreg-te", NULL);
	ret = x9dp_read_pairs(dev, "samsung,link-power-status", iso, 2);
	if (ret)
		return ret;
	d->pd_off = iso[0];
	d->pd_mask = iso[1];
	ret = x9dp_read_pairs(dev, "samsung,phy-isolation", iso, 2);
	ret = ret ?: x9dp_read_pairs(dev, "samsung,lane-map", &d->lane_map[0][0], 8);
	ret = ret ?: x9dp_read_pairs(dev, "samsung,two-lane-lanes", &d->two_lanes[0][0], 4);
	if (ret)
		return ret;
	d->iso_off = iso[0];
	d->iso_mask = iso[1];
	for (i = 0; i < 8; i++)
		if (d->lane_map[i / 4][i % 4] > 3)
			return dev_err_probe(dev, -EINVAL, "samsung,lane-map: lanes are 0-3\n");
	for (i = 0; i < 2; i++) {
		u32 a = d->two_lanes[i][0], b = d->two_lanes[i][1];

		if (!((a == 0 && b == 1) || (a == 2 && b == 3)))
			return dev_err_probe(dev, -EINVAL,
					     "samsung,two-lane-lanes: <0 1> or <2 3> per orientation\n");
	}

	sw_desc.drvdata = d;
	sw_desc.fwnode = dev_fwnode(dev);
	sw_desc.set = x9dp_sw_set;
	d->sw = typec_switch_register(dev, &sw_desc);
	if (IS_ERR(d->sw))
		return dev_err_probe(dev, PTR_ERR(d->sw), "typec switch\n");
	mux_desc.drvdata = d;
	mux_desc.fwnode = dev_fwnode(dev);
	mux_desc.set = x9dp_mux_set;
	d->mux = typec_mux_register(dev, &mux_desc);
	if (IS_ERR(d->mux)) {
		typec_switch_unregister(d->sw);
		return dev_err_probe(dev, PTR_ERR(d->mux), "typec mux\n");
	}

	d->dbg = debugfs_create_dir("exynos9810-dp", NULL);
	debugfs_create_file("state", 0400, d->dbg, d, &x9dp_state_fops);
	debugfs_create_file("step", 0200, d->dbg, d, &x9dp_step_fops);
	debugfs_create_file("m4regs", 0400, d->dbg, d, &x9dp_m4regs_fops);
	ret = devm_add_action_or_reset(dev, x9dp_unregister, d);
	if (ret)
		return ret;
	dev_info(dev, "registered (PHY isolation PMU %#x mask %#x); hardware untouched until a step%s\n",
		 d->iso_off, d->iso_mask,
		 auto_bringup ? " or a DP pin state" : pma_early ? " or a DP state (pma_early)" : "");
	return 0;
}

static const struct of_device_id x9dp_of_match[] = {
	{ .compatible = "samsung,exynos9810-dp" },
	{ }
};
MODULE_DEVICE_TABLE(of, x9dp_of_match);

static struct platform_driver x9dp_driver = {
	.probe = x9dp_probe,
	.driver = {
		.name = "exynos9810-dp",
		.of_match_table = x9dp_of_match,
	},
};
module_platform_driver(x9dp_driver);

MODULE_DESCRIPTION("Exynos 9810 DisplayPort TX: combo PHY DP mode and AUX (bring-up)");
MODULE_LICENSE("GPL");
