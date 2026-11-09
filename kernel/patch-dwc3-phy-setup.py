#!/usr/bin/env python3
"""dwc3: write GUSB2PHYCFG/GUSB3PIPECTL again after phy_init.

  patch-dwc3-phy-setup.py <linux tree>

dwc3_core_init() programs the PHY configuration registers (dwc3_phy_setup:
GUSB2PHYCFG SUSPHY/ENBLSLPM/U2_FREECLK_EXISTS/PHYIF, GUSB3PIPECTL) and only
then calls phy_init(). The Exynos 9810 PHY's init is the vendor CAL
(phy_exynos_usb_v3p1_enable), which resets the link first (USBCON CLKRST
LINK_SW_RST), and that puts those dwc3 registers back to their reset values:
on the phone GUSB2PHYCFG reads 0x40102400, U2_FREECLK_EXISTS still set
although the DT asks snps,dis-u2-freeclk-exists-quirk, ENBLSLPM clear.
Samsung's kernel runs the PHY enable before dwc3_phy_setup(), so the
vendor's controller always has the configured values; this restores that
order by running dwc3_phy_setup() once more right after phy_init(). The
first call stays, for PHYs that need the configuration before their init.
Idempotent; the anchor is checked.
"""
from pathlib import Path
import sys

path = Path(sys.argv[1]) / 'drivers/usb/dwc3/core.c'
s = path.read_text()
MARK = 'zboot: dwc3_phy_setup after phy_init'
if MARK in s:
    sys.exit(0)
old = '''	ret = dwc3_phy_init(dwc);
	if (ret)
		goto err_exit_ulpi;

	ret = dwc3_core_soft_reset(dwc);
	if (ret)
		goto err_exit_phy;
'''
new = '''	ret = dwc3_phy_init(dwc);
	if (ret)
		goto err_exit_ulpi;

	/*
	 * zboot: dwc3_phy_setup after phy_init. A PHY init that resets the
	 * link (Exynos 9810: USBCON LINK_SW_RST) wipes GUSB2PHYCFG and
	 * GUSB3PIPECTL; program them again (the vendor's order).
	 */
	ret = dwc3_phy_setup(dwc);
	if (ret)
		goto err_exit_phy;

	ret = dwc3_core_soft_reset(dwc);
	if (ret)
		goto err_exit_phy;
'''
if s.count(old) != 1:
    sys.exit(f'patch-dwc3-phy-setup: anchor found {s.count(old)}x in {path}')
path.write_text(s.replace(old, new))
print('patch-dwc3-phy-setup: dwc3_phy_setup() again after phy_init()')
