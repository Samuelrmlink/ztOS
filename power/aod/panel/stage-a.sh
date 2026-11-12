#!/bin/bash
# Stage (a) of the panel AOD (ROADMAP 8.45): Samsung's HLPM entry while the
# shell keeps drawing its AOD, A/B against the AOD as it is today.
#   sudo bash stage-a.sh OUTDIR [ROW] [MINUTES]	(inside ztlock run aod-panel)
# ROW 0-3 = HLPM 2/10/30/60 nit (default 0). Needs lpm-watch.sh and
# deadman.sh running. Order per block: today's AOD for MINUTES, HLPM for
# MINUTES, today's AOD again; each with `current_now` once a second after a
# 60 s settle. The sequences are star_a3_s0_alpm_enter_cmdtbl and
# _alpm_exit_cmdtbl, byte for byte (power/aod/PANEL-AOD.md, table A); no
# 76/FE/7D. Ends with a screen on/off so the full brightness set goes again.
set -u
OUT=$1 ROW=${2:-0} MIN=${3:-3}
DEV=/sys/devices/platform/soc@0/16030000.decon
BAT=/sys/class/power_supply/max170xx_battery
RT=/run/user/1000
mkdir -p "$OUT"
NITS=('BB 05 0C BB 4F 4F 84' 'BB 05 0C 9B EE 4F 84' 'BB 05 0C 6B 3F 4F 84' 'BB 05 0C 0B 10 4F 04')
MODE=('53 03' '53 02' '53 02' '53 02')
ENTER1='F0 5A 5A; B1 0B 74; F7 03; F0 A5 A5'
ENTER2="F0 5A 5A; ${NITS[$ROW]}; FC 5A 5A; B0 2B; FD 03; FC A5 A5; ${MODE[$ROW]}; F4 EB 23 0B 17 8A; F7 03; F0 A5 A5"
EXIT='F0 5A 5A; BB 01; F4 EB 23 0B 17 CA; 53 00; F0 A5 A5'
CLOSE='F0 A5 A5; FC A5 A5'
say() { echo "$(date '+%T') $*" | tee -a "$OUT/stage-a.log"; }
lit() { grep -q '^enabled' /sys/class/drm/card*-DSI-*/enabled; }
state() { grep -E '^(screen|aod|aod_frames) ' $RT/ztmetro.state | tr '\n' ' '; }
ctl() { echo "$1" > $RT/ztmetro.ctl; }
in_lpm=0
leave() {
	if [ $in_lpm = 1 ]; then
		echo "$EXIT" > $DEV/panel_cmd; echo "$CLOSE" > $DEV/panel_cmd
		say "exit sent"; in_lpm=0
	fi
}
trap 'say "interrupted"; leave; exit 1' INT TERM
sample() {	# label: 60 s settle, then MIN*60-60 readings
	sleep 60
	local n=$(( MIN * 60 - 60 )) i
	for ((i = 0; i < n; i++)); do
		echo "$1 $(date +%s) $(cat $BAT/current_now) $(cat $BAT/voltage_now) $(cat $BAT/current_avg)" >> "$OUT/samples.txt"
		[ -e /var/tmp/aodp/tripped ] && { say "watcher tripped"; return 1; }
		sleep 1
	done
}
rm -f /var/tmp/aodp/tripped
say "start row $ROW ($MIN min a block); charger $(cat /sys/class/power_supply/max77705-charger/online); battery $(cat $BAT/capacity) %; $(state)"
grep -q '^screen off' $RT/ztmetro.state && grep -q '^aod on' $RT/ztmetro.state && lit ||
	{ say "the shell's AOD is not showing: nothing sent"; exit 1; }
f0=$(grep '^aod_frames' $RT/ztmetro.state)
sample today1 || exit 1
echo "$ENTER1" > $DEV/panel_cmd; sleep 0.02; echo "$ENTER2" > $DEV/panel_cmd; in_lpm=1
say "HLPM entry sent"
sample hlpm || { leave; exit 1; }
f1=$(grep '^aod_frames' $RT/ztmetro.state)
say "frames during: $f0 -> $f1; $(state)"
leave
sample today2 || exit 1
say "done; screen on/off for the brightness set"
ctl 'screen on'; sleep 3; ctl 'screen off'; sleep 2
say "$(state); dmesg tail:"; dmesg | tail -5 >> "$OUT/stage-a.log"
awk '{s[$1]+=$3; n[$1]++; v[$1]+=$4} END {for (k in s) printf "%s: %d samples, current %.1f mA, %.0f mW\n", k, n[k], s[k]/n[k]/1000, s[k]/n[k]*v[k]/n[k]/1e9}' "$OUT/samples.txt" | tee -a "$OUT/stage-a.log"
