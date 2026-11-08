#!/bin/bash
# aod-test2.sh: the AOD spec of 2026-11-08 afternoon, through the shell's ctl (run under ztlock).
# Log: /var/tmp/aod-test2.log. Config keys are changed only for a moment, from a backup, and put back.
R=/run/user/1000
CTL=$R/ztmetro.ctl
LOG=/var/tmp/aod-test2.log
BL=/sys/class/backlight/s9p-panel
C=$HOME/.config/ztmetro/config
SLOG=$HOME/.local/state/ztmetro.log
: > $LOG
cp -p $C /var/tmp/aod-test2-config.bak
ctl() { printf '%s\n' "$1" > $CTL; }
log() { echo "$(date +%T) $*" >> $LOG; }
st() { grep -E '^(screen|aod|aod_frames) ' $R/ztmetro.state | tr '\n' ' '; }
dpms() { cat /sys/class/drm/card*-DSI-1/dpms; }
expect() {
	local s="$(st) dpms $(dpms) bl $(cat $BL/brightness)"
	if echo "$s" | grep -qE "$2"; then log "PASS $1 ($s)"; else log "FAIL $1: want /$2/, have ($s)"; fi
}
key() { sed -i "/^$1 *=/d" $C; [ -n "$2" ] && echo "$1 = $2" >> $C; pkill -HUP -x ztmetro; sleep 1.5; }
restore() { cp -p /var/tmp/aod-test2-config.bak $C; pkill -HUP -x ztmetro; sleep 1.5; }
PID=$(pgrep -x ztmetro)
L0=$(wc -l < $SLOG)
log "start: ztmetro $PID, battery $(cat /sys/class/power_supply/max170xx_battery/capacity)%, config aod=$(grep '^aod ' $C)"
was=$(grep '^screen ' $R/ztmetro.state)

# 1. the power key with AOD on: the AOD, not the dark
ctl "screen on"; sleep 3
BL_ON=$(cat $BL/brightness)
ctl "key 116 down"; sleep 0.1; ctl "key 116 up"; sleep 3
expect "1 power key locks into the AOD" "screen off aod on .*dpms On"

# 3. woken onto the lock screen and left: the AOD after 10 s; a touch restarts the 10 s
ctl "key 116 down"; sleep 0.1; ctl "key 116 up"; sleep 2
expect "3 woken" "screen on"
sleep 6; ctl "tap 180 300"; sleep 6
expect "3 a tap at 6 s: still on at 12 s" "screen on"
sleep 7
expect "3 then the AOD 10 s after the tap" "screen off aod on"
# unlocked: the screen's own timeout (the config's, minutes), not 10 s
ctl "key 116 down"; sleep 0.1; ctl "key 116 up"; sleep 2
ctl "down 3 180 650"; for y in 600 500 400 300 200 100; do ctl "move 3 180 $y"; sleep 0.02; done; ctl "up 3"; sleep 2
if tail -n +$L0 $SLOG | grep -q "unlocked"; then log "unlocked by a swipe"; fi
sleep 13
expect "3 unlocked: still on after 15 s" "screen on"
ctl "key 116 down"; sleep 0.1; ctl "key 116 up"; sleep 3
expect "3 locked again: the AOD" "screen off aod on"

# 2. touches: one tap does nothing; double-tap switched off: nothing either
ctl "tap 180 400"; sleep 1
expect "2 one tap on the AOD: nothing" "screen off aod on"
ctl "tap 180 400"; sleep 0.5; ctl "tap 182 402"; sleep 1
expect "2 a slow double tap (0.5 s): nothing" "screen off aod on"
ctl "tap 180 400"; sleep 0.15; ctl "tap 180 440"; sleep 1
expect "2 a spread double tap (40 points, 160 px): nothing" "screen off aod on"
ctl "tap 60 400"; sleep 0.15; ctl "tap 300 400"; sleep 1
expect "2 a double tap across the screen: nothing" "screen off aod on"
ctl "down 4 180 400"; sleep 0.3; ctl "up 4"; sleep 0.1; ctl "tap 182 402"; sleep 1
expect "2 a slow press then a tap: nothing" "screen off aod on"
ctl "down 5 180 400"; ctl "move 5 180 420"; ctl "up 5"; sleep 0.1; ctl "tap 180 420"; sleep 1
expect "2 a short swipe then a tap: nothing" "screen off aod on"
key double_tap_wake off
ctl "tap 180 400"; sleep 0.15; ctl "tap 182 402"; sleep 2
expect "2 double tap, switched off: nothing" "screen off aod on"
key double_tap_wake on
ctl "tap 180 400"; sleep 0.15; ctl "tap 182 402"; sleep 2
expect "2 double tap, switched on: wakes" "screen on"
ctl "key 116 down"; sleep 0.1; ctl "key 116 up"; sleep 3

# low battery: shown by default; dark only with the switch
ctl "aod test battery 5"; sleep 2
expect "low battery, switch off (default): shown" "screen off aod on"
key aod_low_battery_off on
expect "low battery, switch on: dark" "screen off aod off .*dpms Off"
key aod_low_battery_off ""
ctl "aod test battery real"; sleep 2
expect "battery real: shown" "screen off aod on"

# 4. charger while on the lock screen, with only-while-charging
key aod charging
ctl "aod test charger on"; sleep 1
ctl "key 116 down"; sleep 0.1; ctl "key 116 up"; sleep 2
expect "4 woken on a charger" "screen on"
ctl "aod test charger off"; sleep 2
expect "4 charger out on the lock screen: still on" "screen on"
ctl "aod test charger on"; sleep 13
expect "4 charger in again, lock timeout: the AOD" "screen off aod on"
ctl "aod test charger off"; sleep 3
expect "4 charger out on the AOD (only while charging): dark" "screen off aod off"
ctl "aod test charger real"
key aod on
expect "Always again: shown" "screen off aod on"

# brightness from the room (simulated lux): level by level
for lux in 0 20 150 1500 20000 3 0; do
	ctl "aod test lux $lux"; sleep 4
	log "lux $lux: backlight $(cat $BL/brightness)"
done
ctl "aod test lux real"; sleep 6
log "lux real ($(cut -d' ' -f1 /run/ztoscsf-sensors/light)): backlight $(cat $BL/brightness), sensord $(tr '\n' ' ' < /run/ztoscsf-sensors/status)"
key aod_auto_brightness off
log "auto brightness off: backlight $(cat $BL/brightness), sensord $(tr '\n' ' ' < /run/ztoscsf-sensors/status)"
key aod_auto_brightness ""

# 4. a notification while on the AOD: a new frame, no wake
F0=$(grep aod_frames $R/ztmetro.state | cut -d' ' -f2)
ID=$(notify-send -p -a "AOD test" "AOD test" "a notification while the AOD shows" 2>/dev/null)
sleep 3
F1=$(grep aod_frames $R/ztmetro.state | cut -d' ' -f2)
expect "4 notification: no wake" "screen off aod on"
[ "${F1:-0}" -gt "${F0:-0}" ] && log "PASS 4 notification: a new AOD frame ($F0 -> $F1)" || log "FAIL 4 notification: frames $F0 -> $F1"
if [ -n "$ID" ]; then
	gdbus call --session --dest org.freedesktop.Notifications --object-path /org/freedesktop/Notifications \
		--method org.freedesktop.Notifications.CloseNotification "$ID" > /dev/null 2>&1
	sleep 3
	F2=$(grep aod_frames $R/ztmetro.state | cut -d' ' -f2)
	log "notification closed: frames $F1 -> $F2"
else
	log "notify-send gave no id"
fi

restore
tail -n +$L0 $SLOG | grep -E "AOD not shown|aod: [0-9]+ lux|double tap|unlocked|ERROR" | cut -c1-150 | sed 's/^/    /' >> $LOG
[ "$(pgrep -x ztmetro)" = "$PID" ] && log "PASS ztmetro still $PID" || log "FAIL ztmetro is now $(pgrep -x ztmetro)"
if [ "$was" = "screen on" ]; then ctl "screen on"; fi
log "done: $(st)"
