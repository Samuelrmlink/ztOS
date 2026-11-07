#!/bin/bash
# Save the phone's BOOT, RECOVERY and EFS partitions into stock/, from its running
# TWRP. scripts/flash.sh will not write anything until they are there.
#
#   scripts/backup-stock.sh
#
# Only reads the phone: nothing on it is written, and no partition is mounted. A
# file already in stock/ is kept, never replaced, because the first copy is the
# one nearest to stock. Run this BEFORE the first install: afterwards BOOT holds
# ztOS, and the backup would not be Android's.
#
# EFS holds the phone's IMEI and its modem calibration, which can't be made again.
# Copy stock/ somewhere safe, and keep it private: never share or commit it (the
# repository ignores it). CPEFS, which some phones have, is saved too if present.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ADB=${ADB:-adb}
STOCK=$ROOT/stock
umask 077

sh() { "$ADB" shell "$@" | tr -d '\r'; }

[ "$("$ADB" get-state 2> /dev/null)" = recovery ] || { echo "the phone is not in TWRP over adb" >&2; exit 1; }
[ "$(sh getprop ro.product.device)" = starlte ] || { echo "not a starlte" >&2; exit 1; }
mkdir -p "$STOCK"

save() {	# PARTITION
	local part=$1 by=/dev/block/platform/11120000.ufs/by-name/$1 out=$STOCK/$1.img
	local size want got
	if [ -s "$out" ]; then
		echo "$part: stock/$part.img is already there, keeping it"
		return
	fi
	size=$(sh blockdev --getsize64 "$by")
	case $size in ''|*[!0-9]*|0) echo "$part: cannot read its size ($size)" >&2; return 1 ;; esac
	echo "$part: reading $size bytes"
	want=$(sh sha256sum "$by" | cut -d' ' -f1)
	"$ADB" exec-out "dd if=$by bs=1048576 2>/dev/null" > "$out.part"
	got=$(sha256sum "$out.part" | cut -d' ' -f1)
	if [ "$(stat -c %s "$out.part")" != "$size" ] || [ "$got" != "$want" ]; then
		rm -f "$out.part"
		echo "$part: the copy does not match the phone's, nothing kept" >&2
		return 1
	fi
	mv "$out.part" "$out"
	echo "$part: stock/$part.img sha256=$got (compared with the phone)"
}

for part in BOOT RECOVERY EFS; do
	save "$part"
done
if [ -n "$(sh "ls /dev/block/platform/11120000.ufs/by-name/CPEFS 2>/dev/null")" ]; then
	save CPEFS
fi
echo "BACKUP_OK in $STOCK. Keep a copy off this computer: EFS can't be made again."
