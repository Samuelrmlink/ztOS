#!/bin/bash
# aod-test.sh: the always-on display's rules, through the shell's ctl. Run on the phone as samuel,
# detached; the log is /var/tmp/aod-test.log. Ends with the screen on and the session's aod from the config.
R=/run/user/1000
CTL=$R/ztmetro.ctl
LOG=/var/tmp/aod-test.log
BL=/sys/class/backlight/s9p-panel
: > $LOG
ctl() { printf '%s\n' "$1" > $CTL; }
log() { echo "$(date +%T) $*" >> $LOG; }
st() { grep -E '^(screen|aod|aod_frames) ' $R/ztmetro.state | tr '\n' ' '; }
dpms() { cat /sys/class/drm/card*-DSI-1/dpms; }
snap() { log "$1: $(st)| dpms $(dpms) | backlight $(cat $BL/brightness) | ztmetro.brightness $(cat $R/ztmetro.brightness)"; }
expect() {	# label, want-regex
	local s="$(st) dpms $(dpms)"
	if echo "$s" | grep -qE "$2"; then log "PASS $1 ($s)"; else log "FAIL $1: want /$2/, have ($s)"; fi
}
PID=$(pgrep -x ztmetro)
log "start, ztmetro $PID, battery $(cat /sys/class/power_supply/max170xx_battery/capacity)%, charger $(cat /sys/class/power_supply/max77705-charger/online)"

ctl "screen on"; sleep 3
BL_ON=$(cat $BL/brightness)
snap "awake"

# 1. switched off: exactly the old dark screen, and the glass does nothing
ctl "aod off"; sleep 1
ctl "screen off"; sleep 3
snap "aod off, screen off"
expect "aod off: dark" "screen off aod off .*dpms Off"
ctl "tap 180 400"; sleep 0.15; ctl "tap 180 400"; sleep 2
expect "aod off: a double tap does nothing" "screen off aod off .*dpms Off"
ctl "screen on"; sleep 3
expect "aod off: awake again" "screen on aod off .*dpms On"
[ "$(cat $BL/brightness)" = "$BL_ON" ] && log "PASS brightness back ($BL_ON)" || log "FAIL brightness $(cat $BL/brightness), was $BL_ON"

# 2. always: shown, at the panel's floor
ctl "aod on"; sleep 1
ctl "screen off"; sleep 3
snap "aod on, screen off"
expect "aod on: shown" "screen off aod on .*dpms On"
log "backlight while shown: $(cat $BL/brightness) (awake $BL_ON)"

# 3. one tap, a long touch, a swipe: nothing
ctl "tap 180 400"; sleep 1
expect "one tap: nothing" "screen off aod on"
ctl "down 1 180 400"; sleep 0.6; ctl "up 1"; sleep 0.1; ctl "tap 180 400"; sleep 1
expect "a long touch then a tap: nothing" "screen off aod on"
ctl "down 2 180 400"; ctl "move 2 180 300"; ctl "up 2"; sleep 0.1; ctl "tap 180 300"; sleep 1
expect "a swipe then a tap: nothing" "screen off aod on"
ctl "tap 100 400"; sleep 0.2; ctl "tap 300 400"; sleep 1
expect "two taps far apart: nothing" "screen off aod on"
ctl "tap 180 400"; sleep 0.8; ctl "tap 180 400"; sleep 1
expect "two taps 0.8 s apart: nothing" "screen off aod on"

# 4. a double tap wakes it
ctl "tap 180 400"; sleep 0.15; ctl "tap 185 405"; sleep 3
expect "double tap wakes" "screen on aod off .*dpms On"
[ "$(cat $BL/brightness)" = "$BL_ON" ] && log "PASS brightness back ($BL_ON)" || log "FAIL brightness $(cat $BL/brightness), was $BL_ON"

# 5. the power key wakes it
ctl "screen off"; sleep 3
expect "shown again" "screen off aod on"
ctl "key 116 down"; sleep 0.1; ctl "key 116 up"; sleep 3
expect "power key wakes" "screen on aod off"
[ "$(cat $BL/brightness)" = "$BL_ON" ] && log "PASS brightness back ($BL_ON)" || log "FAIL brightness $(cat $BL/brightness), was $BL_ON"

# 6. home wakes it (the force press sends the same key)
ctl "screen off"; sleep 3
ctl "key 172 down"; sleep 0.1; ctl "key 172 up"; sleep 3
expect "home wakes" "screen on aod off"

# 7. only while charging (the charger simulated)
ctl "aod charging"; sleep 1
ctl "screen off"; sleep 3
expect "charging mode on battery: dark" "screen off aod off .*dpms Off"
ctl "aod test charger on"; sleep 3
expect "charger in: shown" "screen off aod on .*dpms On"
log "backlight while shown: $(cat $BL/brightness)"
ctl "aod test charger off"; sleep 3
expect "charger out: dark" "screen off aod off .*dpms Off"
ctl "aod test charger on"; sleep 3
expect "charger in again: shown" "screen off aod on"

# 8. always, on battery below 15 %: still shown (the cutoff is a switch, off by
# default; aod-test2.sh tries it switched on)
ctl "aod test charger off"; ctl "aod on"; sleep 3
expect "always on battery: shown" "screen off aod on"
ctl "aod test battery 14"; sleep 3
expect "14 %, cutoff off: shown" "screen off aod on"
ctl "aod test battery 14"; ctl "aod test charger on"; sleep 3
expect "14 % on a charger: shown" "screen off aod on"
ctl "aod test battery real"; ctl "aod test charger real"; sleep 2
expect "real again: shown" "screen off aod on"

# 9. a frame each minute
F0=$(grep aod_frames $R/ztmetro.state | cut -d' ' -f2)
sleep $((65 - $(date +%-S)))
F1=$(grep aod_frames $R/ztmetro.state | cut -d' ' -f2)
sleep 60
F2=$(grep aod_frames $R/ztmetro.state | cut -d' ' -f2)
log "frames $F0 -> $F1 -> $F2 over two minute turns"
[ $((F2 - F0)) -ge 2 ] && log "PASS a frame a minute" || log "FAIL frames $F0 $F1 $F2"

ctl "screen on"; sleep 3
expect "awake at the end" "screen on aod off"
[ "$(cat $BL/brightness)" = "$BL_ON" ] && log "PASS brightness back ($BL_ON)" || log "FAIL brightness $(cat $BL/brightness), was $BL_ON"
pkill -HUP -x ztmetro	# the config's aod again
[ "$(pgrep -x ztmetro)" = "$PID" ] && log "PASS ztmetro still $PID" || log "FAIL ztmetro now $(pgrep -x ztmetro), was $PID"
log "done"
