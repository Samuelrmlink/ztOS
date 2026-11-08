#!/bin/bash
# under ztlock: the always-on display for MIN minutes (23), a frame each minute, the 10-minute moves,
# and a wake now and then (power, double tap, home). Log: /var/tmp/aod-soak.log
R=/run/user/1000
CTL=$R/ztmetro.ctl
LOG=/var/tmp/aod-soak.log
MIN=${1:-23}
ctl() { printf '%s\n' "$1" > $CTL; }
log() { echo "$(date +%T) $*" >> $LOG; }
st() { grep -E '^(screen|aod|aod_frames) ' $R/ztmetro.state | tr '\n' ' '; }
PID=$(pgrep -x ztmetro)
was=$(grep '^screen ' $R/ztmetro.state)
LINES=$(wc -l < ~/.local/state/ztmetro.log)
log "soak starts: ztmetro $PID, $was, battery $(cat /sys/class/power_supply/max170xx_battery/capacity)%"
ctl "aod on"; sleep 1; ctl "screen off"; sleep 3
for m in $(seq 1 $MIN); do
	sleep $((60 - $(date +%-S) + 5))
	log "minute $m: $(st)| backlight $(cat /sys/class/backlight/s9p-panel/brightness)"
	case $m in
	6) ctl "key 116 down"; sleep 0.1; ctl "key 116 up"; sleep 8; log "woken by power: $(st)"; ctl "screen off"; sleep 3 ;;
	12) ctl "tap 180 500"; sleep 0.15; ctl "tap 182 502"; sleep 8; log "woken by a double tap: $(st)"; ctl "screen off"; sleep 3 ;;
	18) ctl "key 172 down"; sleep 0.1; ctl "key 172 up"; sleep 8; log "woken by home: $(st)"; ctl "screen off"; sleep 3 ;;
	esac
	[ "$(pgrep -x ztmetro)" = "$PID" ] || log "FAIL ztmetro is now $(pgrep -x ztmetro)"
done
tail -n +$((LINES + 1)) ~/.local/state/ztmetro.log | grep -E "aod|screen o|ERROR|error" | sed 's/^/    /' >> $LOG
pkill -HUP -x ztmetro; sleep 1	# the config's aod again
if [ "$was" = "screen on" ]; then ctl "screen on"; fi
log "soak done: ztmetro $(pgrep -x ztmetro) (was $PID), $(st)"
