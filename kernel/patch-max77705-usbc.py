#!/usr/bin/env python3
"""The MAX77705 USB-C controller driver (kernel/max77705-usbc.c) in the tree.

Copies the driver to drivers/power/supply/max77705_usbc.c (every run, so the
tree always has the current version) and adds its Kconfig symbol
CHARGER_MAX77705_USBC and Makefile line once. It replaces the port's polling
s9p-max77705-muic for the --power image: make-dts.py gives the 0x25 node the
new compatible, so the old driver (still built) binds to nothing.
"""
from pathlib import Path
import shutil
import sys

tree = Path(sys.argv[1])
here = Path(__file__).resolve().parent
MARK = 'zboot: max77705 usbc'

shutil.copyfile(here / 'max77705-usbc.c', tree / 'drivers/power/supply/max77705_usbc.c')


def patch(rel, anchor, new):
    p = tree / rel
    s = p.read_text()
    if MARK in s:
        return
    if s.count(anchor) != 1:
        sys.exit(f'patch-max77705-usbc: anchor changed in {rel}: {anchor[:60]!r}')
    p.write_text(s.replace(anchor, anchor + new))


patch('drivers/power/supply/Kconfig', '''config CHARGER_MAX77705
	tristate "Maxim MAX77705 battery charger driver"
	depends on MFD_MAX77705
	help
	  Say Y to enable support for the Maxim MAX77705 battery charger.
''', '''
# zboot: max77705 usbc
config CHARGER_MAX77705_USBC
	tristate "Maxim MAX77705 USB-C controller (Galaxy S9)"
	depends on MFD_MAX77705 && I2C
	depends on TYPEC || !TYPEC
	help
	  The USB-C MCU in the MAX77705 at I2C 0x25: charger type (BC1.2,
	  proprietary, Type-C Rp, AFC/QC2, USB-PD) as a power_supply, the
	  D+/D- switch, and the Type-C port when TYPEC is enabled.
''')

patch('drivers/power/supply/Makefile', 'obj-$(CONFIG_CHARGER_MAX77705)	+= max77705_charger.o\n',
      '''# zboot: max77705 usbc
obj-$(CONFIG_CHARGER_MAX77705_USBC)	+= max77705_usbc.o
''')
print('patch-max77705-usbc: ok')
