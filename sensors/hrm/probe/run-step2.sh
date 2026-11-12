#!/bin/sh
# Step 2 (owner-approved 2026-11-12): 10 s of IR + red PPG with a fingertip on
# the sensor, through zrn-hrm.ko built with OWN_BUS=1 (its own class-0 bus 104).
# Usage on the phone: ztlock run hrm 'sh run-step2.sh DIR'   (DIR has zrn-hrm.ko)
# Leaves DIR/rec.bin (24-byte zrn_hrm_sample records).
D=${1:-$HOME/hrm-step2}
ACPM=/usr/local/libexec/ztos-power/s9p-acpm
cap=$(cat /sys/class/power_supply/max170xx_battery/capacity)
[ "$cap" -ge 15 ] || { echo "battery $cap% < 15%, not running"; exit 3; }
if lsmod | grep -q '^zrn_cam'; then
	sudo fuser /dev/zrn-cam >/dev/null 2>&1 && { echo "zrn-cam in use"; exit 4; }
fi
grep -q '^i2c_algo_bit' /proc/modules || { echo "i2c-algo-bit not loaded"; exit 5; }
[ -e /sys/bus/i2c/devices/i2c-97 ] || { echo "bus 97 missing"; exit 6; }
grep -qE '^(zrn_nfc|zrn_i2c20|zrn_hrm) ' /proc/modules && { echo "zrn_nfc/zrn_i2c20/zrn_hrm loaded"; exit 8; }
for b in 103 104; do [ -e /sys/bus/i2c/devices/i2c-$b ] && { echo "i2c-$b present"; exit 8; }; done
if sudo grep -E 'gpp5-[01]\)' /sys/kernel/debug/pinctrl/10830000.pinctrl-samsung-pinctrl/pinmux-pins | grep -v 'GPIO UNCLAIMED'; then
	echo "gpp5-0/1 claimed"; exit 9
fi
if sudo awk '/^gpiochip/ {g = /gpg1:/} g && /^ gpio-6 /' /sys/kernel/debug/gpio | grep .; then
	echo "gpg1-6 requested by something"; exit 9
fi
echo "battery $cap%, $(TZ=America/Chicago date)"

ldo_read() { sudo python3 "$ACPM" read 1 0x6f | sed -n 's/.*val=\(0x[0-9a-fA-F]*\).*/\1/p'; }
ldo38=$(ldo_read)
echo "LDO38 0x6f = $ldo38"
[ "$(printf %d "$ldo38")" -eq $((0x2c)) ] || { echo "LDO38 not 0x2c, not running"; exit 10; }
cleanup() {
	grep -q '^zrn_hrm ' /proc/modules && { sudo rmmod zrn_hrm; echo "rmmod: $?"; }
	sudo python3 "$ACPM" write 1 0x6f "$ldo38"
	echo "LDO38 0x6f <- $ldo38 (restore), reads $(ldo_read)"
	echo "gpg1-6: $(sudo grep -E 'gpg1-6\)' /sys/kernel/debug/pinctrl/10430000.pinctrl-samsung-pinctrl/pins)"
}
trap cleanup EXIT
sudo python3 "$ACPM" update 1 0x6f 0x80 0xc0
now=$(ldo_read)
echo "LDO38 0x6f update 0x80/0xc0, reads $now"
[ "$(printf %d "$now")" -eq $((0xac)) ] || { echo "LDO38 did not read 0xac, stopping"; exit 11; }
sleep 0.005

mark=$(awk '{print $1}' /proc/uptime)
sudo insmod "$D/zrn-hrm.ko" || { echo "insmod failed"; exit 12; }
rm -f "$D/rec.bin"
sudo timeout 10 cat /dev/zrn_hrm > "$D/rec.bin"
echo "captured $(stat -c %s "$D/rec.bin") bytes ($(( $(stat -c %s "$D/rec.bin") / 24 )) records)"
sudo rmmod zrn_hrm; echo "rmmod: $?"
sudo dmesg | grep 'zrn-hrm' | awk -v m="$mark" '{t=$0; sub(/^\[ */,"",t); sub(/\].*/,"",t); if (t+0 >= m-1) print}'
