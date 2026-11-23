#!/usr/bin/env python3
"""Patch btbcm.c so the Galaxy S9's BCM4361 comes up.

  patch-btbcm.py <linux source tree>

Its firmware answers LE Read Transmit Power (0x204b) with one byte, and the
host then fails setup with -38 ("unexpected cc 0x204b length: 1 < 3").
btbcm already knows such controllers, but finds them through DMI tables,
which an arm64 phone does not have: the quirk is set for this board too, by
its device-tree compatible.

The UART speed is not patched here. This script used to give hci_bcm.c a
3 Mbaud fallback for starlte, but that code never ran: with btattach the
line discipline sets the operating speed to the tty's speed (hci_ldisc.c,
hu->oper_speed = c_ospeed), which comes first, so the link stayed at 115200
(ROADMAP 8.58). The speed is now btattach's -S, from the device tree's
bluetooth-board max-speed (devices/samsung-starlte/.../zrn-bluetooth). A tree
patched by the old script keeps the dead fallback, which does no harm.

Asserted, so a changed upstream file fails instead of silently building
without it.
"""
import sys
from pathlib import Path

path = Path(sys.argv[1]) / "drivers/bluetooth/btbcm.c"
src = path.read_text()
old = "\tif (dmi_first_match(disable_broken_read_transmit_power))\n"
new = ("\tif (dmi_first_match(disable_broken_read_transmit_power) ||\n"
       "\t    of_machine_is_compatible(\"samsung,starlte\"))\n")
if new not in src:
    assert src.count(old) == 1, "btbcm.c changed: look at the quirk again"
    src = src.replace(old, new)
    if "#include <linux/of.h>" not in src:
        src = src.replace("#include <linux/module.h>\n", "#include <linux/module.h>\n#include <linux/of.h>\n", 1)
    path.write_text(src)
