#!/bin/sh
# Run zrn-hrm-probe.ko once on the phone, with the owner's conditions checked.
# Usage on the phone: ztlock run hrm 'sh run-probe.sh /path/to/zrn-hrm-probe.ko'
KO=${1:-$HOME/zrn-hrm-probe.ko}
h=$(TZ=America/Chicago date +%H)
[ "$h" -ge 7 ] || { echo "before 07:00 CST, not running"; exit 2; }
cap=$(cat /sys/class/power_supply/max170xx_battery/capacity)
[ "$cap" -ge 15 ] || { echo "battery $cap% < 15%, not running"; exit 3; }
if lsmod | grep -q '^zrn_cam'; then
	if sudo fuser /dev/zrn-cam >/dev/null 2>&1; then
		echo "zrn-cam is in use, not running"; exit 4
	fi
fi
grep -q '^i2c_algo_bit' /proc/modules || { echo "i2c-algo-bit not loaded"; exit 5; }
[ -e /sys/bus/i2c/devices/i2c-97 ] || { echo "bus 97 missing (zrn-flash?)"; exit 6; }
[ -e /sys/bus/i2c/devices/i2c-99 ] && { echo "bus 99 already exists"; exit 7; }
# research/nfc may hold gpp5-0/1 (its own i2c-gpio bus for the NFC chip at 0x27)
grep -q '^zrn_nfc' /proc/modules && { echo "zrn_nfc loaded, gpp5 busy"; exit 8; }
if sudo grep -E 'gpp5-[01]\)' /sys/kernel/debug/pinctrl/10830000.pinctrl-samsung-pinctrl/pinmux-pins | grep -v 'GPIO UNCLAIMED'; then
	echo "gpp5-0/1 claimed, not running"; exit 9
fi
if sudo awk '/^gpiochip/ {in5 = /gpp5:/} in5 && /^ gpio-[01] /' /sys/kernel/debug/gpio | grep .; then
	echo "gpp5-0/1 requested, not running"; exit 9
fi
echo "battery $cap%, $(TZ=America/Chicago date)"
mark=$(awk '{print $1}' /proc/uptime)
sudo insmod "$KO"; echo "insmod: $? (failure expected: the module never stays)"
sudo dmesg | grep 'zrn-hrm-probe' | awk -v m="$mark" '{t=$0; sub(/^\[ */,"",t); sub(/\].*/,"",t); if (t+0 >= m-1) print}'
sudo grep -E 'gpp5-[01]\)' /sys/kernel/debug/pinctrl/10830000.pinctrl-samsung-pinctrl/pins
