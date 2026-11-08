#!/usr/bin/env python3
"""The IDT P9320 wireless power receiver (kernel/p9320_wireless.c).

Copies the driver into drivers/power/supply/ and adds CHARGER_P9320 to its
Kconfig and Makefile. build-boot.sh turns the symbol on with --power. The
driver and what it does on a pad: the comment at the top of the C file and
docs in charging/ (wireless.md, wireless-stage2.md).
"""
from pathlib import Path
import shutil
import sys

tree = Path(sys.argv[1])
here = Path(__file__).resolve().parent
sup = tree / 'drivers/power/supply'
MARK = 'zboot: p9320'

src = here / 'p9320_wireless.c'
dst = sup / 'p9320_wireless.c'
if not dst.exists() or dst.read_bytes() != src.read_bytes():
    shutil.copyfile(src, dst)


def patch(path, anchor, new):
    s = path.read_text()
    if MARK in s:
        return False
    if s.count(anchor) != 1:
        sys.exit(f'patch-p9320: anchor changed in {path.name}: {anchor[:60]!r}')
    path.write_text(s.replace(anchor, anchor + new))
    return True


k = patch(sup / 'Kconfig', '''config CHARGER_MAX77705
	tristate "Maxim MAX77705 battery charger driver"
	depends on MFD_MAX77705
	help
	  Say Y to enable support for the Maxim MAX77705 battery charger.
''', '''
# zboot: p9320
config CHARGER_P9320
	tristate "IDT P9320 wireless power receiver"
	depends on I2C && OF && GPIOLIB
	help
	  The Galaxy S9's wireless charging receiver (Samsung "mfc-charger"):
	  receiver-side readings, stock's FOD calibration and Samsung's TX id
	  handshake, and optional Samsung fast wireless (10 V).
''')
m = patch(sup / 'Makefile', 'obj-$(CONFIG_CHARGER_MAX77705)	+= max77705_charger.o\n',
          '# zboot: p9320\nobj-$(CONFIG_CHARGER_P9320)	+= p9320_wireless.o\n')
print('patch-p9320: ' + ('applied' if k or m else 'already applied'))
