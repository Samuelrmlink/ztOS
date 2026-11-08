#!/bin/bash
# aod-power.sh: what the always-on display costs, measured by the fuel gauge
# with NO cable in (then the gauge's current is the whole phone).
#
#   run as root on the phone, detached:  setsid -f bash aod-power.sh [ROUNDS]
#
# Needs the shell from feature/aod (its ctl takes "aod on|off"). It waits for
# the cable to come out (up to 15 minutes), then alternates the dark screen and
# the always-on display, ROUNDS times (3), 20 one-second readings each after 15
# s to settle. Nothing else is changed: Wi-Fi, the hub and the cores stay as
# the phone has them, so the difference is the display's own. The result is in
# /var/tmp/aod-power.log. At the end the session's aod is put back to the
# config's (a reload), the screen stays off, and the phone buzzes twice: then
# plug it in.
#
# Sleep is not involved: with the screen dark and sleep switched on the phone
# might sleep, and the dark reading would then be a sleep reading. The script
# refuses to run with /var/lib/zrn-sleep/enabled present.
. /usr/lib/ztos/user.sh
ROUNDS=${1:-3}
LOG=/var/tmp/aod-power.log
CHG=/sys/class/power_supply/max77705-charger/online
BAT=/sys/class/power_supply/max170xx_battery
BL=/sys/class/backlight/s9p-panel
CTL=$ZTOS_RUNTIME_DIR/ztmetro.ctl
STATE=$ZTOS_RUNTIME_DIR/ztmetro.state
: > "$LOG"
say() { echo "$*" >> "$LOG"; }
shell() { printf '%s\n' "$1" > "$CTL"; }

if [ -e /var/lib/zrn-sleep/enabled ]; then
	say "sleep is switched on: the dark reading would be a sleep reading. Stopped."
	exit 1
fi
say "aod-power $(date)  battery $(cat $BAT/capacity) % $(cat $BAT/voltage_now) uV"
say "waiting for the cable to come out"
n=0
while [ "$(cat $CHG)" != 0 ]; do
	sleep 2; n=$((n + 1))
	[ $n -gt 450 ] && { say "no unplug in 15 minutes: stopped"; exit 1; }
done
say "unplugged at $(date +%T)"

# the charger's own reading of the system current (mA), when its hwmon has one
ISYS=
for h in /sys/class/hwmon/hwmon*; do
	if [ "$(cat $h/name 2>/dev/null)" = max77705 ] && [ "$(cat $h/curr2_label 2>/dev/null)" = ISYS_REG ]; then
		ISYS=$h/curr2_input
	fi
done
SAMPLES=${SAMPLES:-20}	# one-second readings each

reading() {	# label: average battery current (mA, negative = from the battery)
	local label=$1 sum=0 isys=0 v i
	sleep 15
	for i in $(seq "$SAMPLES"); do
		v=$(cat $BAT/current_now); sum=$((sum + v))
		[ -n "$ISYS" ] && isys=$((isys + $(cat $ISYS)))
		sleep 1
	done
	say "$(printf '%-28s %7d mA  ISYS %4d mA  (%s mV, %s %%, backlight %s, %s)' "$label" \
		$((sum / SAMPLES / 1000)) $((isys / SAMPLES)) \
		"$(($(cat $BAT/voltage_now) / 1000))" "$(cat $BAT/capacity)" "$(cat $BL/brightness)" \
		"$(grep -E '^(screen|aod) ' "$STATE" | tr '\n' ' ')")"
}

shell "aod off"; sleep 1; shell "screen off"
for r in $(seq "$ROUNDS"); do
	shell "aod off"
	reading "round $r: dark"
	shell "aod on"
	reading "round $r: always-on display"
done
shell "aod off"
pkill -HUP -x ztmetro	# the config's aod again
say "done $(date +%T)"
shell "haptic heavy"; sleep 0.6; shell "haptic heavy"
