#!/bin/bash
# aod-power2.sh: the current with the screen dark, and with the AOD at its dark-room, lit-room
# and sun levels, the room's light simulated through the ctl. Cable out, nobody touching it,
# run as samuel under the phone lock. ROUNDS (2) rounds of 15 s settling and SAMPLES (50)
# one-second readings each. Log: /var/tmp/aod-power2.log. The AOD goes back to the config.
R=/run/user/1000
CTL=$R/ztmetro.ctl
BAT=/sys/class/power_supply/max170xx_battery
BL=/sys/class/backlight/s9p-panel
LOG=/var/tmp/aod-power2.log
ctl() { printf '%s\n' "$1" > $CTL; }
was=$(grep '^screen ' $R/ztmetro.state)
ISYS=
for h in /sys/class/hwmon/hwmon*; do
	[ "$(cat $h/name)" = max77705 ] && [ "$(cat $h/curr2_label 2>/dev/null)" = ISYS_REG ] && ISYS=$h/curr2_input
done
SAMPLES=${SAMPLES:-50}
reading() {	# LABEL: one line, mA from the battery (positive) and the charger's ISYS
	local label=$1 sum=0 isys=0 i
	sleep 15
	for i in $(seq "$SAMPLES"); do
		sum=$((sum + $(cat $BAT/current_now)))
		[ -n "$ISYS" ] && isys=$((isys + $(cat $ISYS)))
		sleep 1
	done
	echo "$(printf '%-26s %6d mA  ISYS %4d mA  (%s mV, %s %%, backlight %3s, %s)' "$label" \
		$((-sum / SAMPLES / 1000)) $((isys / SAMPLES)) $(($(cat $BAT/voltage_now) / 1000)) \
		"$(cat $BAT/capacity)" "$(cat $BL/brightness)" \
		"$(grep -E '^(screen|aod) ' $R/ztmetro.state | tr '\n' ' ')")" >> $LOG
}
echo "aod power $(date)  battery $(cat $BAT/capacity) %, charger $(cat /sys/class/power_supply/max77705-charger/online), Wi-Fi associated (power save)" > $LOG
if [ "$(cat /sys/class/power_supply/max77705-charger/online)" != 0 ]; then
	echo "on a charger: the gauge does not see the phone's current. Stopped." >> $LOG
	exit 2
fi
ctl "screen on"; sleep 2
ctl "aod off"; sleep 1; ctl "screen off"; sleep 2
for r in $(seq ${ROUNDS:-2}); do
	ctl "aod off"
	reading "round $r: dark"
	ctl "aod test lux 0"; ctl "aod on"
	reading "round $r: AOD dark room (~2 nits)"
	ctl "aod test lux 100"
	reading "round $r: AOD lit room (~7 nits)"
	ctl "aod test lux 20000"
	reading "round $r: AOD sun (~55 nits)"
	ctl "aod test lux real"
done
pkill -HUP -x ztmetro; sleep 2	# the config's aod again
[ "$was" = "screen on" ] && ctl "screen on"
echo "done $(date +%T)" >> $LOG
