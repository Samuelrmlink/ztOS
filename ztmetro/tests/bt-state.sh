#!/bin/sh
# make check-bt-list: ztos-bt-state against a fake BlueZ (tests/bt-fake). An LE
# GATT connection counts as connected; a PropertiesChanged that matters is
# acted on at once, one that does not (RSSI) is not; the file is written only
# when it changes; and its dbus-monitor goes when it does.
BIN=$(readlink -f "$1")
HERE=$(dirname "$(readlink -f "$0")")
T=$(mktemp -d)
PID=
trap '[ -n "$PID" ] && kill $PID 2>/dev/null; sleep 0.2; rm -rf "$T"' EXIT
cp -r "$HERE/bt-fake" "$T/bt"
mkdir -p "$T/run"
export FAKE_BT="$T/bt" FAKE_BT_LOG="$T/calls" XDG_RUNTIME_DIR="$T/run"
export PATH="$T/bt/bin:$PATH"
# the timer far off: only BlueZ's signals can explain a quick change
export ZTOS_BT_STATE_ON_S=60
F=$T/run/ztos-bt

fail() { echo "bt-state: FAIL: $*"; cat "$F" 2>/dev/null; exit 1; }
has() { grep -qx "$1" "$F" 2>/dev/null; }
# the file should come to say this within 3 s
expect() {
	i=0
	until has "$1"; do
		i=$((i + 1))
		[ $i -gt 150 ] && fail "$2: no '$1'"
		sleep 0.02
	done
}
emit() { printf '%s\n' "$@" > "$T/bt/emit.new"; mv "$T/bt/emit.new" "$T/bt/emit"; }
looks() { grep -c '^show' "$T/calls" 2>/dev/null || echo 0; }

"$BIN" > "$T/log" 2>&1 & PID=$!

# the Chameleon: LE only, GATT, connected
expect "connected=ChameleonUltra" "LE device"
has "on=1" || fail "powered"
has "mac=E3:19:D9:61:F8:3B" || fail "mac"
has "macs=E3:19:D9:61:F8:3B" || fail "macs"
has "battery=100" || fail "battery"
has "apple=0" || fail "apple"

# the bus filters what wakes it: devices, not a GATT tool's notifications
i=0
until [ -s "$T/bt/dbus-args" ]; do i=$((i + 1)); [ $i -gt 150 ] && fail "no dbus-monitor"; sleep 0.02; done
grep -q "arg0='org.bluez.Device1'" "$T/bt/dbus-args" || fail "no Device1 match"
grep -q "member='PropertiesChanged'" "$T/bt/dbus-args" || fail "no PropertiesChanged match"
grep -q "GattCharacteristic" "$T/bt/dbus-args" && fail "listens to GATT values"

# it goes; an RSSI change says nothing about that, Connected does
: > "$T/bt/connected"
before=$(looks)
emit 'signal time=1 sender=:1.46 -> destination=(null destination) serial=1 path=/org/bluez/hci0/dev_FE_E8_F4_FB_65_43; interface=org.freedesktop.DBus.Properties; member=PropertiesChanged' \
	'   string "org.bluez.Device1"' '   array [' '      dict entry(' '         string "RSSI"' \
	'         variant             int16 -70' '      )' '   ]'
sleep 0.5
[ "$(looks)" = "$before" ] || fail "an RSSI change made it look"
has "connected=ChameleonUltra" || fail "changed without a look"
emit 'signal time=2 sender=:1.46 -> destination=(null destination) serial=2 path=/org/bluez/hci0/dev_E3_19_D9_61_F8_3B; interface=org.freedesktop.DBus.Properties; member=PropertiesChanged' \
	'   string "org.bluez.Device1"' '   array [' '      dict entry(' '         string "Connected"' \
	'         variant             boolean false' '      )' '   ]'
expect "connected=" "disconnect, by signal"
has "macs=" || fail "macs after disconnect"

# a look that finds nothing new leaves the file alone
inode=$(ls -i "$F" | cut -d' ' -f1)
before=$(looks)
emit '         string "Connected"'
i=0
until [ "$(looks)" != "$before" ]; do i=$((i + 1)); [ $i -gt 150 ] && fail "no look on Connected"; sleep 0.02; done
sleep 0.3
[ "$(ls -i "$F" | cut -d' ' -f1)" = "$inode" ] || fail "rewritten with nothing new"

# back, with a BR/EDR device too: both listed, the first named
printf 'Device E3:19:D9:61:F8:3B ChameleonUltra\nDevice 00:0D:44:EB:50:54 UE MINI BOOM\n' > "$T/bt/connected"
emit '         string "Connected"'
expect "macs=E3:19:D9:61:F8:3B,00:0D:44:EB:50:54" "two connected"
has "connected=ChameleonUltra" || fail "first connected"

# it goes, and its dbus-monitor with it
mon=$(cat "$T/bt/dbus-pid")
kill $PID; wait $PID 2>/dev/null; PID=
i=0
while kill -0 "$mon" 2>/dev/null; do
	i=$((i + 1)); [ $i -gt 100 ] && fail "dbus-monitor outlived it"; sleep 0.02
done
echo "PASS: ztos-bt-state follows BlueZ, LE links included"
