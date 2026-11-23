#!/bin/sh
# Closed-loop identify test with the owner's Chameleon Ultra (owner-approved
# 2026-11-22). Runs on the phone. Holds sleep off with a zrn-sleepd inhibit
# (mtime = expiry) and restores the active slot to 4 on every exit path.
#   chameleon-test.sh <dir with modules+ztnfc+stage2.sh>
set -u
D=${1:?dir}
CU=~/cubridge/cubridge
INH=/run/zrn-sleepd/inhibit/nfc-test
SECS=20
say() { echo "== $*"; }

renew() { printf 'label=nfc-test\n' > "$INH"; touch -d "@$(( $(date +%s) + 300 ))" "$INH"; }
cleanup() {
	say "restore: active slot -> 4"
	$CU set-active-slot 4 >/dev/null 2>&1
	$CU info 2>/dev/null | grep -E '"active_slot"'
	rm -f "$INH"
	say "inhibit removed"
}
trap cleanup EXIT INT TERM

# sleep must already be awake; confirm BLE link up
st=$($CU status 2>&1)
echo "$st" | grep -q '"link": "connected"' || { say "STOP: cubridge not connected"; echo "$st" | head -8; exit 2; }
mkdir -p /run/zrn-sleepd/inhibit 2>/dev/null
renew
say "inhibit armed: $(ls -l --time-style=+%s $INH | awk '{print $6}') expiry"

scan() {
	say "identify scan ($1)"
	ztlock run nfc "sudo -n sh $D/stage2.sh $D $SECS identify" 2>&1
}

say "slot 8 (Ultralight MF0ICU1)"
$CU set-active-slot 8 >/dev/null
$CU info 2>/dev/null | grep -E '"active_slot"|"device_mode"'
renew
scan ultralight

say "slot 4 (MIFARE Classic 1K)"
$CU set-active-slot 4 >/dev/null
$CU info 2>/dev/null | grep -E '"active_slot"'
renew
scan classic
# cleanup (slot 4 + inhibit removal) runs on EXIT
