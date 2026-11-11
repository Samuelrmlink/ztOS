#!/usr/bin/env python3
"""Exynos 9810 USB 2.0 PHY: host mode (USB OTG).

  patch-usbphy-host.py <linux tree>

Patches the port's drivers/phy/samsung/phy-exynos9810-usbdrd.c (installed by
apply_drivers.sh) with a phy_ops.set_mode. dwc3 calls phy_set_mode() with
PHY_MODE_USB_HOST when its role switch goes to host and PHY_MODE_USB_DEVICE
when it comes back.

This board has no VBUS pad (vendor is_not_vbus_pad = 1), so init forces VBUS
valid and BVALID for the gadget. The vendor's dwc3_otg_start_host() then calls
phy_set(SET_DPDM_PULLDOWN) -> phy_exynos_usb_v3p1_config_host_mode() (Samsung
4.9 kernel, drivers/phy/samsung/phy-exynos-usb3p1.c), which this mirrors:

  host:   USBCON_UTMI  |= DP_PULLDOWN | DM_PULLDOWN,
                       &= ~(FORCE_BVALID | FORCE_VBUSVALID)
          USBCON_HSP   &= ~(VBUSVLDEXTSEL | VBUSVLDEXT)
  device: init's values back: pull-downs off, VBUS/BVALID forced valid.

HSP_TUNE follows the mode too (vendor exynos_usbdrd_utmi_tune()): the host
column of the stock hs_tune node in host mode (compdis 7: the HS disconnect
threshold only a host uses), the device column back in device mode.

The 15 kOhm D+/D- pull-downs are what a host must present; without them a
device's pull-up is never seen as a connect. Only PHY registers of the SoC are
touched here; VBUS itself comes from the MAX77705 charger. Idempotent; every
anchor is checked.
"""
from pathlib import Path
import sys

path = Path(sys.argv[1]) / 'drivers/phy/samsung/phy-exynos9810-usbdrd.c'
s = path.read_text()
MARK = 'zboot: host mode'
if MARK in s:
    sys.exit(0)


def sub(old, new):
    global s
    if s.count(old) != 1:
        sys.exit(f'patch-usbphy-host: expected 1x {old[:60]!r}, found {s.count(old)}')
    s = s.replace(old, new)


sub('''	struct phy *phy;
};
''', '''	struct phy *phy;
	/* zboot: host mode { */
	bool inited;			/* between init and exit */
	bool host;			/* the last set_mode was PHY_MODE_USB_HOST */
	/* } */
};
''')

sub('''static int s9p_usbphy_init(struct phy *phy)
{''', '''/*
 * zboot: host mode. phy_exynos_usb_v3p1_config_host_mode() for host; the
 * forced VBUS/BVALID of init (no VBUS pad on this board) for device.
 *
 * The HS tuning follows the mode, as the vendor's exynos_usbdrd_utmi_tune()
 * does when the host starts: the host column of the stock hs_tune node
 * (tune_value = <dev host>, exynos9810-starlte DT): tx_pre_emp 3, tx_vref 7,
 * rx_sqrx 6, compdis 7, tx_res 1, tx_rise 1. compdis is the HS disconnect
 * threshold, which only a host uses; with the device value 3 the port drops
 * right after the chirp ("Cannot enable").
 */
#define S9P_HSP_TUNE_HOST	(HSP_TUNE_TXVREF(7) | HSP_TUNE_TXRISE(1) | \\
				 HSP_TUNE_TXRES(1) | HSP_TUNE_TXPREEMPA(3) | \\
				 HSP_TUNE_SQRX(6) | HSP_TUNE_COMPDIS(7))

static void s9p_usbphy_apply_mode(struct s9p_usbphy *p)
{
	void __iomem *r = p->regs;

	if (p->host) {
		rmw(r + USBCON_UTMI, UTMI_FORCE_BVALID | UTMI_FORCE_VBUSVALID,
		    UTMI_DP_PULLDOWN | UTMI_DM_PULLDOWN);
		rmw(r + USBCON_HSP, HSP_VBUSVLDEXTSEL | HSP_VBUSVLDEXT, 0);
		if (s9p_phy_tune)
			rmw(r + USBCON_HSP_TUNE, S9P_HSP_TUNE_MASK, S9P_HSP_TUNE_HOST);
	} else {
		rmw(r + USBCON_UTMI, UTMI_DP_PULLDOWN | UTMI_DM_PULLDOWN,
		    UTMI_FORCE_BVALID | UTMI_FORCE_VBUSVALID);
		rmw(r + USBCON_HSP, 0, HSP_VBUSVLDEXTSEL | HSP_VBUSVLDEXT);
		if (s9p_phy_tune)
			rmw(r + USBCON_HSP_TUNE, S9P_HSP_TUNE_MASK, S9P_HSP_TUNE_VAL);
	}
	dev_dbg(p->dev, "%s: utmi=%08x hsp=%08x hsp_tune=%08x\\n",
		p->host ? "host" : "device", readl(r + USBCON_UTMI),
		readl(r + USBCON_HSP), readl(r + USBCON_HSP_TUNE));
}

static int s9p_usbphy_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct s9p_usbphy *p = phy_get_drvdata(phy);
	bool host;

	switch (mode) {
	case PHY_MODE_USB_HOST:
	case PHY_MODE_USB_HOST_HS:
	case PHY_MODE_USB_HOST_FS:
	case PHY_MODE_USB_HOST_LS:
		host = true;
		break;
	case PHY_MODE_USB_DEVICE:
	case PHY_MODE_USB_DEVICE_HS:
	case PHY_MODE_USB_DEVICE_FS:
	case PHY_MODE_USB_DEVICE_LS:
	case PHY_MODE_USB_OTG:
		host = false;
		break;
	default:
		return -EINVAL;
	}
	if (host != p->host)
		dev_info(p->dev, "%s mode\\n", host ? "host" : "device");
	p->host = host;
	if (p->inited)
		s9p_usbphy_apply_mode(p);	/* else init applies it */
	return 0;
}

static int s9p_usbphy_init(struct phy *phy)
{''')

sub('''	if (s9p_phy_tune)
		rmw(r + USBCON_HSP_TUNE, S9P_HSP_TUNE_MASK, S9P_HSP_TUNE_VAL);

	s9p_usbphy_dump(p, "after init");''', '''	if (s9p_phy_tune)
		rmw(r + USBCON_HSP_TUNE, S9P_HSP_TUNE_MASK, S9P_HSP_TUNE_VAL);

	/* zboot: host mode survives a re-init (resume) */
	p->inited = true;
	if (p->host)
		s9p_usbphy_apply_mode(p);

	s9p_usbphy_dump(p, "after init");''')

sub('''	/* phy_exynos_usb_v3p1_disable() */
	rmw(r + USBCON_UTMI, 0, UTMI_FORCE_SUSPEND | UTMI_FORCE_SLEEP);''',
    '''	p->inited = false;	/* zboot: host mode */
	/* phy_exynos_usb_v3p1_disable() */
	rmw(r + USBCON_UTMI, 0, UTMI_FORCE_SUSPEND | UTMI_FORCE_SLEEP);''')

sub('''	.power_off	= s9p_usbphy_power_off,
''', '''	.power_off	= s9p_usbphy_power_off,
	.set_mode	= s9p_usbphy_set_mode,	/* zboot: host mode */
''')

path.write_text(s)
print('patch-usbphy-host: set_mode (host: D+/D- pull-downs, VBUS not forced)')
