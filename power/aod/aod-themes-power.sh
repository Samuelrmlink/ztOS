#!/bin/bash
# aod-themes-power.sh: the phone's current with each AOD theme showing, at the lit-room level
# (lux simulated: 100), and the dark for comparison. Cable out, nobody touching it, run as
# samuel under the phone lock. SAMPLES (40) one-second readings after 12 s, each.
# Log: /var/tmp/aod-themes-power.log. The AOD and its theme go back to the config.
R=/run/user/1000
CTL=$R/ztmetro.ctl
BAT=/sys/class/power_supply/max170xx_battery
LOG=/var/tmp/aod-themes-power.log
ctl() { printf '%s\n' "$1" > $CTL; }
was=$(grep '^screen ' $R/ztmetro.state)
SAMPLES=${SAMPLES:-40}
reading() {
	local sum=0 i
	sleep 12
	for i in $(seq "$SAMPLES"); do sum=$((sum + $(cat $BAT/current_now))); sleep 1; done
	printf '%-10s %5d mA  (%s %%)\n' "$1" $((-sum / SAMPLES / 1000)) "$(cat $BAT/capacity)" >> $LOG
}
echo "aod themes power $(date)  battery $(cat $BAT/capacity) %, lit-room level (lux 100)" > $LOG
[ "$(cat /sys/class/power_supply/max77705-charger/online)" != 0 ] && { echo "on a charger: stopped" >> $LOG; exit 2; }
ctl "screen on"; sleep 2
ctl "aod off"; sleep 1; ctl "screen off"; sleep 2
reading dark
ctl "aod test lux 100"; ctl "aod on"
for t in metro compact digital analogue stacked calendar flip image; do
	ctl "aod theme $t"
	reading "$t"
done
ctl "aod test lux real"
pkill -HUP -x ztmetro; sleep 2	# the config's aod and theme again
[ "$was" = "screen on" ] && ctl "screen on"
grep -E "aod: theme" ~/.local/state/ztmetro.log | tail -8 | sed 's/^/    /' >> $LOG
echo "done $(date +%T)" >> $LOG
