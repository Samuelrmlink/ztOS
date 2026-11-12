#!/bin/sh
# Stage 2 (card reading), owner-approved 2026-11-12. Run as root inside
# `ztlock run nfc`, only after the owner's go for the card in place.
#   stage2.sh <dir with zrn-i2c20.ko, zrn-nfc.ko, ztnfc.py> [seconds]
# The kernel side enables CLKOUT1 and LDO38 on open of /dev/zt-nfc and
# restores both on close, whatever happens to ztnfc; this script then
# unloads both modules.
D=${1:?dir}
SECS=${2:-20}
say() { echo "== $*"; }

cap=$(cat /sys/class/power_supply/max170xx_battery/capacity)
say "date $(date -Is) battery ${cap}%"
[ "$cap" -ge 15 ] || { say "STOP: battery below 15%"; exit 1; }
for m in zrn_hrm zrn_nfc zrn_nfc_stage1 zrn_i2c20; do
	lsmod | grep -q "^$m" && { say "STOP: $m loaded"; exit 1; }
done
lsmod | grep -q '^i2c_algo_bit' || { say "STOP: i2c_algo_bit not loaded"; exit 1; }
pm=$(grep -E 'gpp5-[01]\)' /sys/kernel/debug/pinctrl/10830000.pinctrl*/pinmux-pins)
echo "$pm" | grep -q -v UNCLAIMED && { say "STOP: gpp5-0/1 claimed"; exit 1; }

M="zrn-nfc-stage2 $(date +%s)"; echo "$M" > /dev/kmsg
klog() { dmesg | sed -n "/$M/,\$p" | grep -E 'zrn-nfc|zrn-i2c20'; }
cleanup() {
	lsmod | grep -q '^zrn_nfc ' && rmmod zrn_nfc
	lsmod | grep -q '^zrn_i2c20' && rmmod zrn_i2c20
	say "kernel log"
	klog
	grep -E 'gpp5-[01]\)' /sys/kernel/debug/pinctrl/10830000.pinctrl*/pinmux-pins
	lsmod | grep -E '^zrn_nfc|^zrn_i2c20' && say "WARN: module still loaded" || say "modules unloaded"
}
trap cleanup EXIT

insmod "$D/zrn-i2c20.ko" || { say "STOP: zrn-i2c20 did not load"; exit 1; }
insmod "$D/zrn-nfc.ko" || { say "STOP: zrn-nfc did not load"; exit 1; }
say "ztnfc scan, ${SECS} s"
python3 -I "$D/ztnfc.py" scan --seconds "$SECS"
say "ztnfc rc=$?"
