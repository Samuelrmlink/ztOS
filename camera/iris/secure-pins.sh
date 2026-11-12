#!/bin/sh
# Read-only: list the pads that read back as all zeros, including the
# power-down fields, in the pinctrl debugfs. On this phone those are exactly
# the pads Samsung hands to the secure world (fingerprint SPI1, eSE SPI7,
# iris I2C and enable, MST). Run on the phone with sudo. No register is
# written; the reads go through the pinctrl driver like any GPIO read.
#
#	sudo sh camera/iris/secure-pins.sh
set -eu
D=/sys/kernel/debug/pinctrl
[ -d "$D" ] || { echo "debugfs not mounted"; exit 1; }
for f in "$D"/*-pinctrl/pins; do
	grep -E 'CON\(0x0\) DAT\(0x0\) PUD\(0x0\) DRV\(0x0\) CON_PDN\(0x0\) PUD_PDN\(0x0\)$' "$f" |
		sed "s|^|$(basename "$(dirname "$f")" | cut -d. -f1) |" || true
done
