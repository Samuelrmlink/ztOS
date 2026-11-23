#!/bin/bash
# Host-side checks for the Bluetooth UART speed (ROADMAP 8.58):
#   1. zrn-bluetooth picks the speed from a device tree's bluetooth-board
#      max-speed, falls back to 3000000, takes ZRN_BT_SPEED over both, and
#      refuses what the UART or hci_bcm cannot do;
#   2. the speed it picks is one samsung_tty makes from UART_BT's 200 MHz
#      clock within 2 % (its divisor arithmetic, s3c24xx_serial_getclk), and
#      leaves room for A2DP AAC at 320 kbit/s plus a voice link.
#   bluetooth/test-uart-speed.sh
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SCRIPT=$ROOT/devices/samsung-starlte/overlay/usr/local/sbin/zrn-bluetooth
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
check() {	# <what> <expected> <got>
	if [ "$2" = "$3" ]; then echo "ok   $1: $3"; else echo "FAIL $1: expected $2, got $3"; fail=1; fi
}
speed() { ZRN_BT_PRINT_SPEED=1 ZRN_BT_DT="$1" sh "$SCRIPT" | tail -1; }
node() {	# <dt dir> <name> <compatible> [max-speed]
	mkdir -p "$1/$2"
	printf '%s\0' "$3" > "$1/$2/compatible"
	[ -z "${4:-}" ] || printf "$(printf '\\%03o' $(( $4 >> 24 & 255 )) $(( $4 >> 16 & 255 )) $(( $4 >> 8 & 255 )) $(( $4 & 255 )))" > "$1/$2/max-speed"
}

mkdir -p "$T/none"
node "$T/none" usb-sbu-mux gpio-sbu-mux
check "no bluetooth-board node" 3000000 "$(speed "$T/none")"

node "$T/nospeed" bluetooth-board ztos,zrn-bt
check "node without max-speed" 3000000 "$(speed "$T/nospeed")"

node "$T/dt2m" aaa-first ztos,other 921600
node "$T/dt2m" bluetooth-board ztos,zrn-bt 2000000
check "max-speed 2000000 from the node" 2000000 "$(speed "$T/dt2m")"

node "$T/dt4m" bluetooth-board ztos,zrn-bt 4000000
check "max-speed 4000000 refused" 115200 "$(speed "$T/dt4m")"

check "ZRN_BT_SPEED=115200 over the tree" 115200 "$(ZRN_BT_SPEED=115200 speed "$T/dt2m")"
check "ZRN_BT_SPEED=123 refused" 115200 "$(ZRN_BT_SPEED=123 speed "$T/none")"

# the property in the tree make-dts.py writes
if grep -q 'max-speed = <3000000>;' "$ROOT/kernel/make-dts.py"; then
	echo "ok   make-dts.py: bluetooth-board max-speed = <3000000>"
else
	echo "FAIL make-dts.py: no max-speed in bluetooth-board"; fail=1
fi

# samsung_tty's divisor for UART_BT (has_divslot, fracval: 1/16 steps)
python3 -I - "$(speed "$T/none")" <<'PY' || fail=1
import sys
rate, want = 200_000_000, int(sys.argv[1])
div = rate // want                      # s3c24xx_serial_getclk: rate / req_baud
ubrdiv, frac = div // 16 - 1, div & 15
got = rate / (16 * (ubrdiv + 1) + frac)
err = (got - want) / want * 100
payload = want * 8 / 10                 # 8N1
need = 320_000 + 64_000                 # A2DP AAC 320 kbit/s + mSBC voice
print(f"{'ok  ' if abs(err) < 2 else 'FAIL'} {want} baud: UBRDIV {ubrdiv:#x} "
      f"UFRACVAL {frac:#x} -> {got:.0f} ({err:+.2f} %)")
print(f"{'ok  ' if payload > 3 * need else 'FAIL'} payload {payload / 1e6:.1f} Mbit/s "
      f"for {need / 1e3:.0f} kbit/s of audio ({payload / need:.1f}x)")
# what the phone ran before: 115200 -> UBRDIV 0x6b, UFRACVAL 0x8 (read on the phone)
d = rate // 115200
assert (d // 16 - 1, d & 15) == (0x6b, 0x8), "115200 divisor does not match the phone's registers"
print("ok   115200 baud gives UBRDIV 0x6b UFRACVAL 0x8, as read from the phone")
sys.exit(0 if abs(err) < 2 and payload > 3 * need else 1)
PY

[ $fail = 0 ] && echo "UART_SPEED_OK" || { echo "UART_SPEED_FAIL"; exit 1; }
