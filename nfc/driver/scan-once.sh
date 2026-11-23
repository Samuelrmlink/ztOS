#!/bin/sh
# One identify-only scan with the Chameleon on slot 8, then slot 4 restored.
# Holds a zrn-sleepd inhibit for the run; removes it and restores the slot on
# every exit path. Approved commands only (no 0x2F).
#   scan-once.sh <dir>
set -u
D=${1:?dir}
CU=~/cubridge/cubridge
INH=/run/zrn-sleepd/inhibit/nfc-test
say() { echo "== $*"; }
cleanup() {
	say "restore: active slot -> 4"
	$CU set-active-slot 4 2>&1 | grep -E '"ok"|error' | head -2
	$CU info 2>/dev/null | grep -E '"active_slot"'
	rm -f "$INH"; say "inhibit removed"
}
trap cleanup EXIT INT TERM

w=$(cat /sys/class/power_supply/max77705-wireless/online 2>/dev/null)
[ "$w" = 0 ] || { say "STOP: wireless online=$w"; exit 2; }
$CU status 2>&1 | grep -q '"link": "connected"' || { say "STOP: cubridge not connected"; exit 2; }
printf 'label=nfc-test\n' > "$INH"; touch -d "@$(( $(date +%s) + 300 ))" "$INH"
say "inhibit armed"

$CU set-active-slot 8 2>&1 | grep -E '"ok"|error' | head -2
$CU info 2>/dev/null | grep -E '"active_slot"|"device_mode"'
say "identify scan (slot 8)"
sudo -n sh "$D/stage2.sh" "$D" 20 identify 2>&1
