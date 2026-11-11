#!/usr/bin/env python3
"""brcmfmac: keep the BCM4361 associated through system sleep (ROADMAP 8.16).

Mainline's PCIe suspend sends D3_INFORM through the TCM mailbox and the
config-space SBMBX doorbell, and waits for D3_ACK in the TCM. This phone's
firmware (13.50.31.5, PCIe shared version 7, flags 0x30040007: no
PCIE_SHARED_USE_MAILBOX) never looks there: from shared version 6 on,
Samsung's driver (bcmdhd_100_15, dhdpcie_send_mb_data) sends host-to-dongle
mailbox data as a control-ring message (MSG_TYPE_H2D_MAILBOX_DATA, 0x23) and
gets the answer as MSG_TYPE_D2H_MAILBOX_DATA (0x24) on the control complete
ring, unless the firmware asks for the mailbox. Hence "the BCM4361 does not
acknowledge D3" (power/FIRMWARE-AUDIT.md): the request never reached it.

With D3 acknowledged the dongle keeps running with the link down: it stays
associated and buffers what arrives, and asserts WL_HOST_WAKE (gpa0-7 on this
board, an EINT that can wake the SoC) when it has something for the host.
Samsung wakes on that line's rising edge (dhd_custom_exynos.c,
IORESOURCE_IRQ_HIGHEDGE for PCIe). The line comes from the device tree, the
PCI device's own node: host-wake-gpios (a GPIO, not an interrupt, so the
PCI core does not take it for the device's INTx).

What this changes, each part behind a writable brcmfmac parameter:

  d3_ctrlmsg   (Y) mailbox data as control messages when the firmware's shared
               version is 6 or later and it does not set USE_MAILBOX: D3_INFORM
               out, D3_ACK / DS requests / FW halt in. Otherwise as before.
  keep_assoc   (Y) with a host-wake line and a firmware without wowl (this one
               lists no "wowl" in its "cap"), advertise WoWLAN "any" so that
               cfg80211 does not disconnect at suspend (it does when no WoWLAN
               is configured: wiphy_suspend -> cfg80211_leave_all), and suspend
               with the association kept: no wowl iovars, mkeep_alive 30 s as
               mainline's WoWLAN path does. `iw phy0 wowlan enable any` arms it
               (zrn-sleepd does, for the mode that keeps Wi-Fi).

The D3 cycle, as Samsung's dhdpcie_bus_suspend/resume do it:
  suspend  D3_INFORM, wait for D3_ACK (2 s), dongle interrupts masked, the
           host-wake IRQ enabled and wake-armed. The PCI core then puts the
           function in D3hot and the root complex stops the link (PERST#).
  resume   the root complex retrains, the PCI core restores the config space;
           if the dongle still runs (its shared-area pointer at the end of RAM
           is unchanged) it is a hot resume: BAR0 window, the BAR1 sizing
           write, interrupts back, HOSTRDY on doorbell 1 (no D0_INFORM:
           Samsung's use_d0_inform is FALSE). Otherwise the full re-probe
           mainline always did.

/sys/module/brcmfmac/parameters/host_wakes counts the host-wake edges,
d3_acks the acknowledged D3 entries, hot_resumes the resumes that kept the
firmware running. Writing MS to d3_test (root) runs one D3 cycle with the
system awake and the link up: the protocol and the host-wake line on the
bench, without a system sleep.

A dead link after a resume is left alone: an MMIO access over it hangs this
SoC (the root complex's guard covers config space only), so the resume and
a later unbind skip the registers when pci_device_is_present() fails.

Usage: patch-brcmfmac-d3.py <kernel tree>
"""
from pathlib import Path
import sys

D = Path(sys.argv[1]) / 'drivers/net/wireless/broadcom/brcm80211/brcmfmac'
MARK = 'zboot: d3 keep'


def edit(name, pairs):
    p = D / name
    s = p.read_text()
    if MARK in s:
        return False
    for old, new in pairs:
        if s.count(old) != 1:
            sys.exit(f'patch-brcmfmac-d3: {name}: anchor changed:\n{old}')
        s = s.replace(old, new, 1)
    p.write_text(s)
    return True


changed = False

changed |= edit('bus.h', [
    ("""	bool wowl_supported;

	const struct brcmf_bus_ops *ops;""",
     """	bool wowl_supported;
	bool wake_keep;		/* zboot: d3 keep. A host-wake line: the
				 * association can be kept through sleep */

	const struct brcmf_bus_ops *ops;"""),
])

changed |= edit('msgbuf.h', [
    ("""#ifdef CONFIG_BRCMFMAC_PROTO_MSGBUF""",
     """/* zboot: d3 keep. MSG_TYPE_D2H_MAILBOX_DATA's value, handed to the bus */
struct device;
void brcmf_pcie_d2h_mailbox(struct device *dev, u32 data);

#ifdef CONFIG_BRCMFMAC_PROTO_MSGBUF"""),
])

changed |= edit('msgbuf.c', [
    ("""#include <linux/etherdevice.h>
""",
     """#include <linux/etherdevice.h>
#include <linux/unaligned.h>
"""),
    ("""#define MSGBUF_TYPE_LPBK_DMAXFER_CMPLT		0x14
""",
     """#define MSGBUF_TYPE_LPBK_DMAXFER_CMPLT		0x14
/* zboot: d3 keep. Mailbox data on the control rings (shared version >= 6) */
#define MSGBUF_TYPE_H2D_MAILBOX_DATA		0x23
#define MSGBUF_TYPE_D2H_MAILBOX_DATA		0x24
"""),
    ("""	case MSGBUF_TYPE_RX_CMPLT:
		brcmf_dbg(MSGBUF, "MSGBUF_TYPE_RX_CMPLT\\n");
		brcmf_msgbuf_process_rx_complete(msgbuf, buf);
		break;
	default:""",
     """	case MSGBUF_TYPE_RX_CMPLT:
		brcmf_dbg(MSGBUF, "MSGBUF_TYPE_RX_CMPLT\\n");
		brcmf_msgbuf_process_rx_complete(msgbuf, buf);
		break;
	case MSGBUF_TYPE_D2H_MAILBOX_DATA:
		/* common header (8), completion header (4), the value */
		brcmf_pcie_d2h_mailbox(drvr->bus_if->dev,
				       get_unaligned_le32((u8 *)buf + 12));
		break;
	default:"""),
])

changed |= edit('cfg80211.c', [
    ("""static s32 brcmf_cfg80211_suspend(struct wiphy *wiphy,
				  struct cfg80211_wowlan *wowl)
{""",
     """/* zboot: d3 keep. See kernel/patch-brcmfmac-d3.py. */
static bool brcmf_keep_assoc = true;
module_param_named(keep_assoc, brcmf_keep_assoc, bool, 0644);
MODULE_PARM_DESC(keep_assoc, "keep the association through system sleep (host-wake line, WoWLAN any)");

#ifdef CONFIG_PM
static const struct wiphy_wowlan_support brcmf_wowlan_keep = {
	.flags = WIPHY_WOWLAN_ANY,
};
#endif

static s32 brcmf_cfg80211_suspend(struct wiphy *wiphy,
				  struct cfg80211_wowlan *wowl)
{"""),
    ("""	} else {
		/* Configure WOWL parameters */
		brcmf_configure_wowl(cfg, ifp, wowl);
""",
     """	} else if (!brcmf_feat_is_enabled(ifp, BRCMF_FEAT_WOWL)) {
		/* no wowl in the firmware: the association is kept and the
		 * dongle wakes the host through its host-wake line */
		brcmf_dbg(INFO, "suspend: keeping the association\\n");
		brcmf_keepalive_start(ifp, 30);
	} else {
		/* Configure WOWL parameters */
		brcmf_configure_wowl(cfg, ifp, wowl);
"""),
    ("""	if (brcmf_feat_is_enabled(ifp, BRCMF_FEAT_WOWL))
		brcmf_wiphy_wowl_params(wiphy, ifp);
""",
     """	if (brcmf_feat_is_enabled(ifp, BRCMF_FEAT_WOWL))
		brcmf_wiphy_wowl_params(wiphy, ifp);
#ifdef CONFIG_PM
	else if (brcmf_keep_assoc && drvr->bus_if->wake_keep)
		wiphy->wowlan = &brcmf_wowlan_keep;
#endif
"""),
])

changed |= edit('pcie.c', [
    ("""#include <linux/unaligned.h>
""",
     """#include <linux/unaligned.h>
#include <linux/gpio/consumer.h>
#include <linux/pm_wakeup.h>
"""),
    ("""	bool irq_allocated;
	bool wowl_enabled;""",
     """	bool irq_allocated;
	bool wowl_enabled;
	/* zboot: d3 keep */
	struct gpio_desc *host_wake;
	int host_wake_irq;
	struct wakeup_source *host_wake_ws;
	bool d3_kept;			/* D3 acknowledged: the dongle runs on */
	bool link_lost;			/* no link after a resume: no MMIO at all */
	bool mb_ctrlmsg;		/* mailbox data on the control ring */"""),
    # parameters, the control-ring mailbox, the value handler
    ("""static int
brcmf_pcie_send_mb_data(struct brcmf_pciedev_info *devinfo, u32 htod_mb_data)
{""",
     """/* zboot: d3 keep. See kernel/patch-brcmfmac-d3.py. */
static bool brcmf_d3_ctrlmsg = true;
module_param_named(d3_ctrlmsg, brcmf_d3_ctrlmsg, bool, 0644);
MODULE_PARM_DESC(d3_ctrlmsg, "mailbox data as control-ring messages (shared version >= 6)");
static unsigned long brcmf_host_wakes, brcmf_d3_acks, brcmf_hot_resumes;
module_param_named(host_wakes, brcmf_host_wakes, ulong, 0444);
module_param_named(d3_acks, brcmf_d3_acks, ulong, 0444);
module_param_named(hot_resumes, brcmf_hot_resumes, ulong, 0444);
static struct brcmf_pciedev_info *brcmf_pcie_test_dev;	/* d3_test's */

#define BRCMF_PCIE_SHARED_USE_MAILBOX		0x2000000
#define BRCMF_H2D_MAILBOX_DATA_MSGTYPE		0x23

static void brcmf_pcie_mb_value(struct brcmf_pciedev_info *devinfo,
				u32 dtoh_mb_data);
static int brcmf_pcie_send_mb_data(struct brcmf_pciedev_info *devinfo,
				   u32 htod_mb_data);

/* MSG_TYPE_H2D_MAILBOX_DATA: a common header and the value, 40 bytes, on the
 * H2D control submit ring (Samsung's dhd_prot_h2d_mbdata_send_ctrlmsg) */
static int brcmf_pcie_send_mb_ctrlmsg(struct brcmf_pciedev_info *devinfo,
				      u32 data)
{
	struct brcmf_commonring *ring;
	u8 *msg;
	int err;

	if (!devinfo->shared.commonrings[BRCMF_H2D_MSGRING_CONTROL_SUBMIT])
		return -EIO;
	ring = &devinfo->shared.commonrings[BRCMF_H2D_MSGRING_CONTROL_SUBMIT]->commonring;
	brcmf_commonring_lock(ring);
	msg = brcmf_commonring_reserve_for_write(ring);
	if (!msg) {
		brcmf_commonring_unlock(ring);
		return -ENOMEM;
	}
	memset(msg, 0, ring->item_len);
	msg[0] = BRCMF_H2D_MAILBOX_DATA_MSGTYPE;
	put_unaligned_le32(data, msg + 8);
	err = brcmf_commonring_write_complete(ring);
	brcmf_commonring_unlock(ring);
	return err;
}

void brcmf_pcie_d2h_mailbox(struct device *dev, u32 data)
{
	struct brcmf_bus *bus = dev_get_drvdata(dev);

	if (!bus || bus->proto_type != BRCMF_PROTO_MSGBUF || !bus->bus_priv.pcie)
		return;
	brcmf_pcie_mb_value(bus->bus_priv.pcie->devinfo, data);
}

static irqreturn_t brcmf_pcie_host_wake_isr(int irq, void *arg)
{
	struct brcmf_pciedev_info *devinfo = arg;

	brcmf_host_wakes++;
	/* a sleep on its way is given up; the system stays up a moment */
	pm_wakeup_ws_event(devinfo->host_wake_ws, 500, true);
	return IRQ_HANDLED;
}

/* WL_HOST_WAKE from the PCI function's DT node (host-wake-gpios) */
static void brcmf_pcie_host_wake_init(struct brcmf_pciedev_info *devinfo,
				      struct brcmf_bus *bus)
{
	struct device *dev = &devinfo->pdev->dev;
	struct gpio_desc *gpio;
	int irq;

	gpio = gpiod_get_optional(dev, "host-wake", GPIOD_IN);
	if (IS_ERR_OR_NULL(gpio)) {
		if (IS_ERR(gpio))
			brcmf_err(bus, "host-wake gpio: %ld\\n", PTR_ERR(gpio));
		return;
	}
	irq = gpiod_to_irq(gpio);
	if (irq <= 0) {
		brcmf_err(bus, "host-wake gpio has no IRQ (%d)\\n", irq);
		gpiod_put(gpio);
		return;
	}
	devinfo->host_wake_ws = wakeup_source_register(dev, "brcmf_host_wake");
	if (request_irq(irq, brcmf_pcie_host_wake_isr,
			IRQF_TRIGGER_RISING | IRQF_NO_AUTOEN,
			"brcmf_host_wake", devinfo)) {
		brcmf_err(bus, "host-wake IRQ %d not available\\n", irq);
		wakeup_source_unregister(devinfo->host_wake_ws);
		devinfo->host_wake_ws = NULL;
		gpiod_put(gpio);
		return;
	}
	devinfo->host_wake = gpio;
	devinfo->host_wake_irq = irq;
	bus->wake_keep = true;
	dev_info(dev, "host-wake on IRQ %d\\n", irq);
}

static void brcmf_pcie_host_wake_free(struct brcmf_pciedev_info *devinfo)
{
	if (!devinfo->host_wake)
		return;
	if (devinfo->d3_kept) {
		disable_irq_wake(devinfo->host_wake_irq);
		disable_irq(devinfo->host_wake_irq);
		devinfo->d3_kept = false;
	}
	free_irq(devinfo->host_wake_irq, devinfo);
	wakeup_source_unregister(devinfo->host_wake_ws);
	devinfo->host_wake_ws = NULL;
	gpiod_put(devinfo->host_wake);
	devinfo->host_wake = NULL;
}

static int
brcmf_pcie_send_mb_data(struct brcmf_pciedev_info *devinfo, u32 htod_mb_data)
{"""),
    ("""	shared = &devinfo->shared;
	addr = shared->htod_mb_data_addr;
	cur_htod_mb_data = brcmf_pcie_read_tcm32(devinfo, addr);
""",
     """	if (devinfo->mb_ctrlmsg)
		return brcmf_pcie_send_mb_ctrlmsg(devinfo, htod_mb_data);

	shared = &devinfo->shared;
	addr = shared->htod_mb_data_addr;
	cur_htod_mb_data = brcmf_pcie_read_tcm32(devinfo, addr);
"""),
    ("""	brcmf_pcie_write_tcm32(devinfo, addr, 0);

	brcmf_dbg(PCIE, "D2H_MB_DATA: 0x%04x\\n", dtoh_mb_data);""",
     """	brcmf_pcie_write_tcm32(devinfo, addr, 0);
	brcmf_pcie_mb_value(devinfo, dtoh_mb_data);
}

static void brcmf_pcie_mb_value(struct brcmf_pciedev_info *devinfo,
				u32 dtoh_mb_data)
{
	brcmf_dbg(PCIE, "D2H_MB_DATA: 0x%04x\\n", dtoh_mb_data);"""),
    # shared flags: which mailbox
    ("""	shared->flags = brcmf_pcie_read_tcm32(devinfo, sharedram_addr);
	shared->version = (u8)(shared->flags & BRCMF_PCIE_SHARED_VERSION_MASK);""",
     """	shared->flags = brcmf_pcie_read_tcm32(devinfo, sharedram_addr);
	shared->version = (u8)(shared->flags & BRCMF_PCIE_SHARED_VERSION_MASK);
	devinfo->mb_ctrlmsg = brcmf_d3_ctrlmsg && shared->version >= 6 &&
			      !(shared->flags & BRCMF_PCIE_SHARED_USE_MAILBOX);
	dev_info(&devinfo->pdev->dev, "shared version %u flags %#010x: mailbox data %s\\n",
		 shared->version, shared->flags,
		 devinfo->mb_ctrlmsg ? "on the control ring" : "in TCM");"""),
    # probe: the host-wake line before the firmware request
    ("""	fwreq = brcmf_pcie_prepare_fw_request(devinfo);
	if (!fwreq) {
		ret = -ENOMEM;
		goto fail_brcmf;
	}""",
     """	brcmf_pcie_host_wake_init(devinfo, bus);

	fwreq = brcmf_pcie_prepare_fw_request(devinfo);
	if (!fwreq) {
		ret = -ENOMEM;
		goto fail_brcmf;
	}"""),
    ("""fail_brcmf:
	brcmf_free(&devinfo->pdev->dev);""",
     """fail_brcmf:
	brcmf_pcie_host_wake_free(devinfo);
	brcmf_free(&devinfo->pdev->dev);"""),
    ("""	devinfo = bus->bus_priv.pcie->devinfo;
	brcmf_pcie_bus_console_read(devinfo, false);
	brcmf_pcie_fwcon_timer(devinfo, false);
""",
     """	devinfo = bus->bus_priv.pcie->devinfo;
	brcmf_pcie_host_wake_free(devinfo);
	brcmf_pcie_bus_console_read(devinfo, false);
	brcmf_pcie_fwcon_timer(devinfo, false);
"""),
    # D3 entry: the dongle masked, the host-wake line armed
    ("""	if (!devinfo->mbdata_completed) {
		brcmf_err(bus, "Timeout on response for entering D3 substate\\n");
		brcmf_bus_change_state(bus, BRCMF_BUS_UP);
		return -EIO;
	}

	devinfo->state = BRCMFMAC_PCIE_STATE_DOWN;
""",
     """	if (!devinfo->mbdata_completed) {
		brcmf_err(bus, "Timeout on response for entering D3 substate\\n");
		brcmf_bus_change_state(bus, BRCMF_BUS_UP);
		return -EIO;
	}

	devinfo->state = BRCMFMAC_PCIE_STATE_DOWN;
	brcmf_d3_acks++;
	brcmf_pcie_intr_disable(devinfo);
	devinfo->d3_kept = true;
	if (devinfo->host_wake) {
		enable_irq(devinfo->host_wake_irq);
		enable_irq_wake(devinfo->host_wake_irq);
	}
"""),
    # D3 exit: hot when the dongle still runs
    ("""	/* Check if device is still up and running, if so we are ready */
	if (brcmf_pcie_read_reg32(devinfo, devinfo->reginfo->intmask) != 0) {""",
     """	if (devinfo->d3_kept) {
		u32 shared;

		devinfo->d3_kept = false;
		if (devinfo->host_wake) {
			disable_irq_wake(devinfo->host_wake_irq);
			disable_irq(devinfo->host_wake_irq);
		}
		/* an MMIO read over a dead link hangs this SoC (the root
		 * complex's guard covers config space only) */
		if (!pci_device_is_present(devinfo->pdev)) {
			brcmf_err(bus, "D3 exit: no link: Wi-Fi stays down\\n");
			devinfo->link_lost = true;
			return -EIO;
		}
		/* PERST# reset the dongle's PCIe core, not its firmware: the
		 * shared area's pointer at the end of RAM tells which */
		shared = brcmf_pcie_read_ram32(devinfo, devinfo->ci->ramsize - 4);
		if (shared == devinfo->shared.tcm_base_address) {
			brcmf_pcie_attach(devinfo);	/* BAR0 window, BAR1 size */
			devinfo->state = BRCMFMAC_PCIE_STATE_UP;
			if (!devinfo->mb_ctrlmsg &&
			    brcmf_pcie_send_mb_data(devinfo, BRCMF_H2D_HOST_D0_INFORM))
				goto cleanup;
			brcmf_pcie_select_core(devinfo, BCMA_CORE_PCIE2);
			brcmf_bus_change_state(bus, BRCMF_BUS_UP);
			brcmf_pcie_intr_enable(devinfo);
			brcmf_pcie_hostready(devinfo);
			brcmf_pcie_fwcon_timer(devinfo, true);
			brcmf_hot_resumes++;
			/* what arrived meanwhile is on the rings already */
			brcmf_proto_msgbuf_rx_trigger(&devinfo->pdev->dev);
			return 0;
		}
		brcmf_err(bus, "D3 exit: dongle lost (shared %#x, was %#x)\\n",
			  shared, devinfo->shared.tcm_base_address);
		goto cleanup;
	}

	if (!pci_device_is_present(devinfo->pdev)) {
		brcmf_err(bus, "resume: no link: Wi-Fi stays down\\n");
		devinfo->link_lost = true;
		return -EIO;
	}

	/* Check if device is still up and running, if so we are ready */
	if (brcmf_pcie_read_reg32(devinfo, devinfo->reginfo->intmask) != 0) {"""),
    # remove: nothing over a dead link
    ("""	devinfo = bus->bus_priv.pcie->devinfo;
	brcmf_pcie_host_wake_free(devinfo);
	brcmf_pcie_bus_console_read(devinfo, false);
	brcmf_pcie_fwcon_timer(devinfo, false);

	devinfo->state = BRCMFMAC_PCIE_STATE_DOWN;
	if (devinfo->ci)
		brcmf_pcie_intr_disable(devinfo);
""", """	devinfo = bus->bus_priv.pcie->devinfo;
	brcmf_pcie_host_wake_free(devinfo);
	if (!devinfo->link_lost)
		brcmf_pcie_bus_console_read(devinfo, false);
	brcmf_pcie_fwcon_timer(devinfo, false);

	devinfo->state = BRCMFMAC_PCIE_STATE_DOWN;
	if (devinfo->ci && !devinfo->link_lost)
		brcmf_pcie_intr_disable(devinfo);
"""),
    ("""	kfree(bus->msgbuf);
	kfree(bus);

	brcmf_pcie_release_irq(devinfo);
	brcmf_pcie_release_scratchbuffers(devinfo);
	brcmf_pcie_release_ringbuffers(devinfo);
	brcmf_pcie_reset_device(devinfo);
""", """	kfree(bus->msgbuf);
	kfree(bus);

	if (devinfo->link_lost) {
		if (devinfo->irq_allocated)
			free_irq(pdev->irq, devinfo);
		devinfo->irq_allocated = false;
	} else {
		brcmf_pcie_release_irq(devinfo);
	}
	brcmf_pcie_release_scratchbuffers(devinfo);
	brcmf_pcie_release_ringbuffers(devinfo);
	if (!devinfo->link_lost)
		brcmf_pcie_reset_device(devinfo);
"""),
    # the bench's D3 cycle without a system sleep (d3_test)
    ("""	ret = brcmf_attach(&devinfo->pdev->dev);
	if (ret)
		goto fail;
""", """	ret = brcmf_attach(&devinfo->pdev->dev);
	if (ret)
		goto fail;
	brcmf_pcie_test_dev = devinfo;
"""),
    ("""	kfree(bus->msgbuf);
	kfree(bus);

	if (devinfo->link_lost) {""", """	kfree(bus->msgbuf);
	kfree(bus);

	if (brcmf_pcie_test_dev == devinfo)
		brcmf_pcie_test_dev = NULL;
	if (devinfo->link_lost) {"""),
    ("""	devinfo->state = BRCMFMAC_PCIE_STATE_DOWN;
	brcmf_d3_acks++;""", """	devinfo->state = BRCMFMAC_PCIE_STATE_DOWN;
	dev_info(dev, "D3: acknowledged\\n");
	brcmf_d3_acks++;"""),
    ("""			brcmf_hot_resumes++;""", """			brcmf_hot_resumes++;
			dev_info(dev, "D3 exit: firmware kept, association kept\\n");"""),
    ("""#endif /* CONFIG_PM */
""", """#endif /* CONFIG_PM */

#ifdef CONFIG_PM
/* d3_test=MS: one D3 cycle on the bench, the system awake and the link up:
 * D3_INFORM, D3_ACK, MS milliseconds with the dongle in D3 (the host-wake
 * line armed: host_wakes counts what it would have woken for), then the hot
 * exit (HOSTRDY). The protocol alone, without the root complex's sleep. */
static int brcmf_pcie_d3_test_set(const char *val, const struct kernel_param *kp)
{
	struct brcmf_pciedev_info *devinfo = brcmf_pcie_test_dev;
	struct device *dev;
	unsigned int ms;
	int err;

	if (kstrtouint(val, 0, &ms) || ms > 60000)
		return -EINVAL;
	if (!devinfo)
		return -ENODEV;
	dev = &devinfo->pdev->dev;
	device_lock(dev);
	if (brcmf_pcie_test_dev != devinfo) {
		device_unlock(dev);
		return -ENODEV;
	}
	err = brcmf_pcie_pm_enter_D3(dev);
	if (!err) {
		msleep(ms);
		err = brcmf_pcie_pm_leave_D3(dev);
	}
	device_unlock(dev);
	dev_info(dev, "d3_test %u ms: %d\\n", ms, err);
	return err;
}

static const struct kernel_param_ops brcmf_pcie_d3_test_ops = {
	.set = brcmf_pcie_d3_test_set,
};
module_param_cb(d3_test, &brcmf_pcie_d3_test_ops, NULL, 0200);
#endif
"""),
])

if changed:
    print('patch-brcmfmac-d3: applied')
else:
    print('patch-brcmfmac-d3: already applied')
