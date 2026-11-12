#!/bin/sh
# The AOD's cost against the AOD hidden as in a pocket (ROADMAP 8.44), from the
# fuel gauge's charge counter, so the sleep in B is counted. Run detached on
# the phone:
#   nohup sh pocket-ab.sh > /var/tmp/aod-pocket-ab.log 2>&1 &
# It waits (up to WAIT_H hours) for the charger out, the screen off with the
# AOD up and the battery at 15 % or more, then, inside `ztlock run aod-pocket`:
#   A: the AOD showing, A_S seconds
#   B: "pocket covered" to the shell (the AOD hidden; zrn-sleepd may sleep), B_S
# and puts it back ("pocket clear"). It stops at once if the screen comes on, a
# charger comes, or the battery falls under 15 %. Nothing else is changed.
A_S=${A_S:-600}
B_S=${B_S:-1200}
WAIT_H=${WAIT_H:-12}
export XDG_RUNTIME_DIR=/run/user/1000
S=$XDG_RUNTIME_DIR/ztmetro.ctl
B=/sys/class/power_supply/max170xx_battery
CH=/sys/class/power_supply/max77705-charger/online

state() { grep -E "^$1 " $XDG_RUNTIME_DIR/ztmetro.state | cut -d' ' -f2; }
ok() { [ "$(cat $CH)" = 0 ] && [ "$(cat $B/capacity)" -ge 15 ] && [ "$(state screen)" = off ]; }
charge() { cat $B/charge_now 2>/dev/null || cat $B/charge_counter; }
wakes() { sudo zrn-sleepd log all | grep -c woke; }

# Wall-clock time: sleep(1) counts only the time the SoC is awake, so each
# phase ends by the clock, and the gauge's charge covers the sleeps.
phase() {	# name seconds
	c0=$(charge); t0=$(date +%s); n=0; sum=0; w0=$(wakes)
	while [ "$(date +%s)" -lt $((t0 + $2)) ]; do
		ok || { echo "$1: stopped (screen on, a charger, or the battery)"; return 1; }
		sum=$((sum + $(cat $B/current_now))); n=$((n + 1))
		sleep 5
	done
	c1=$(charge); dt=$(($(date +%s) - t0))
	# charge_now is in uAh: mA = uAh * 3.6 / s
	echo "$1: $dt s, charge $c0 -> $c1 uAh = $(( (c0 - c1) * 36 / dt / 10 )) mA average;" \
		"current_now mean $((n ? sum / n / 1000 : 0)) mA over $n awake samples (uA on this gauge);" \
		"aod=$(state aod) covered=$(state aod_covered); $(( $(wakes) - w0 )) wakes"
}

if [ "$1" = --run ]; then
	ok || { echo "not ready any more"; exit 0; }
	echo "A: AOD up, $(date), battery $(cat $B/capacity) %, voltage $(cat $B/voltage_now)"
	phase "A (AOD shown)" "$A_S"
	echo "pocket covered" > $S
	sleep 2
	echo "B: AOD hidden as covered, $(date)"
	phase "B (AOD hidden)" "$B_S"
	echo "pocket clear" > $S
	echo "done $(date), battery $(cat $B/capacity) %"
	sudo zrn-sleepd log 30 | tail -30
	exit 0
fi

echo "pocket-ab: waiting for the charger out, the screen off, the AOD up ($(date))"
limit=$(( $(date +%s) + WAIT_H * 3600 ))
until ok && [ "$(state aod)" = on ]; do
	[ "$(date +%s)" -gt "$limit" ] && { echo "gave up waiting"; exit 0; }
	sleep 60
done
exec ztlock run aod-pocket "A_S=$A_S B_S=$B_S sh $(readlink -f "$0") --run"
