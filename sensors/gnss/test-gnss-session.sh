#!/bin/bash
# GNSS check-up sessions on the phone (ROADMAP 8.29). Root; run each step
# inside the phone lock, one short step at a time:
#   ztlock run gnss 'sudo bash ~/gnss-test/test-gnss-session.sh STEP ...'
# Steps:
#   deploy               install the gate, its unit and ztos-location from
#                        ~/gnss-test (backups: NAME.pre-gnss), restart the gate
#   restore              put the .pre-gnss files back, restart the gate
#   nocsf on|off         a runtime drop-in: ztos-gnss's stop without its
#                        ztoscoresensorframework restart (gone on reboot)
#   session NAME SECS PREP
#                        PREP: asis | hot | warm | cold. Stops the engine
#                        first (location off, then on), sets the aiding up:
#                          hot   store and LTO as they are, 30 s off
#                          warm  no store (gldata.sto aside); LTO, last fix kept
#                          cold  no store, no LTO, no last fix (time from NTP
#                                still injected); all put back afterwards
#                        then 30 baseline current samples with the engine
#                        off, then a reader for SECS seconds (gnss-stats.py)
#                        with a current sample each second.
#   idle SECS            after a session: is anything of GNSS still running
#                        or waking? (unit, gpsd, lhd CPU and context switches,
#                        hub ready, light sensor still updating, crash lines)
# Results: ~/gnss-test/NAME.json (no positions). The raw NMEA, which has
# positions, stays in /run/ztos-gnss-test (root only) and is deleted at the end
# of each session unless KEEP_RAW=1.
set -u
H=${H:-/home/$(id -nu 1000)/gnss-test}
O=/usr/local
D=/var/lib/zrn-android/data/vendor/gps
LAST=/var/lib/ztos-gnss/last
RAW=/run/ztos-gnss-test
B=/sys/class/power_supply/max170xx_battery
log() { echo "$(date +%T) $*"; }

guard() {
	local h cap
	h=$(date +%H); cap=$(cat $B/capacity)
	if [ "$h" -lt 7 ] || [ "$h" -ge 22 ]; then log "not between 07:00 and 22:00 ($h h): stop"; exit 3; fi
	if [ "$cap" -lt 16 ]; then log "battery $cap %: stop"; exit 3; fi
}

engine_off() {
	# location off stops the engine at once (the gate's switch); wait for the unit
	ztos-location off
	for _ in $(seq 40); do systemctl is-active -q ztos-gnss.service || break; sleep 1; done
	systemctl is-active -q ztos-gnss.service && { log "engine still on after 40 s"; return 1; }
	for _ in $(seq 20); do pgrep -f /vendor/bin/hw/gpsd >/dev/null || break; sleep 0.5; done
	return 0
}

samples() {	# N: one current/voltage sample a second, "mA uV" lines
	for _ in $(seq "$1"); do echo "$(cat $B/current_now) $(cat $B/voltage_now 2>/dev/null)"; sleep 1; done
}

case "${1:-}" in
deploy)
	guard
	for f in sbin/ztos-gnss-gate bin/ztos-location; do
		[ -e $O/$f ] && [ ! -e $O/$f.pre-gnss ] && cp -a $O/$f $O/$f.pre-gnss
		install -m 0755 $H/$(basename $f) $O/$f
	done
	U=/etc/systemd/system/ztos-gnss-gate.service
	[ -e $U.pre-gnss ] || cp -a $U $U.pre-gnss
	install -m 0644 $H/ztos-gnss-gate.service $U
	systemctl daemon-reload
	systemctl restart ztos-gnss-gate.service
	sleep 2
	systemctl is-active ztos-gnss-gate.service
	ls -la /run/ztos-gnss/
	cat /run/ztos-gnss/state
	;;
restore)
	for f in sbin/ztos-gnss-gate bin/ztos-location /etc/systemd/system/ztos-gnss-gate.service; do
		p=$f; case $f in /*) ;; *) p=$O/$f ;; esac
		if [ -e $p.pre-gnss ]; then mv -f $p.pre-gnss $p; elif [ $f = bin/ztos-location ]; then rm -f $p; fi
	done
	rm -f /var/lib/ztos-gnss/off
	systemctl daemon-reload
	systemctl restart ztos-gnss-gate.service
	systemctl is-active ztos-gnss-gate.service
	;;
nocsf)
	R=/run/systemd/system/ztos-gnss.service.d
	if [ "${2:-}" = on ]; then
		mkdir -p $R
		printf '[Service]\nExecStopPost=\nExecStopPost=-/bin/rm -f /run/zrn-sleepd/inhibit/timed-gnss\n' > $R/50-gnss-test-no-csf.conf
	else
		rm -f $R/50-gnss-test-no-csf.conf
	fi
	systemctl daemon-reload
	systemctl show -p ExecStopPost ztos-gnss.service | tr ';' '\n' | grep -o 'argv\[\]=[^;]*'
	;;
session)
	guard
	NAME=$2 SECS=$3 PREP=$4
	mkdir -p -m 0700 $RAW
	engine_off || exit 1
	aside=()
	case $PREP in
	hot) sleep 30 ;;
	warm) [ -e $D/gldata.sto ] && mv $D/gldata.sto $RAW/gldata.sto && aside+=(gldata.sto) ;;
	cold)
		[ -e $D/gldata.sto ] && mv $D/gldata.sto $RAW/gldata.sto && aside+=(gldata.sto)
		[ -e $D/lto2.dat ] && mv $D/lto2.dat $RAW/lto2.dat && aside+=(lto2.dat)
		# the gate keeps the last fix in memory too: restart it without one
		[ -e $LAST ] && mv $LAST $RAW/last && aside+=(last)
		systemctl restart ztos-gnss-gate.service; sleep 2
		ztos-location off
		;;
	esac
	log "aiding put aside: ${aside[*]:-none}; store $(ls -la --time-style=+%F_%T $D/gldata.sto 2>/dev/null | awk '{print $6}') lto $(ls -la --time-style=+%F_%T $D/lto2.dat 2>/dev/null | awk '{print $6}')"
	log "baseline: engine off, 30 s"
	samples 30 > $H/$NAME.base
	ztos-location on
	samples "$SECS" > $H/$NAME.gnss &
	sp=$!
	python3 $H/gnss-stats.py live "$SECS" --save $RAW/$NAME.nmea > $H/$NAME.json
	wait $sp
	# engine CPU over the session
	for p in $(pgrep -f /vendor/bin/hw/gpsd) $(pgrep -x lhd); do
		echo "cpu $(cat /proc/$p/comm) $(awk '{print $14 + $15}' /proc/$p/stat) ticks since start"
	done >> $H/$NAME.cpu
	engine_off
	ztos-location on
	# what was put aside goes back (the store a session wrote is newer: kept)
	for f in "${aside[@]}"; do
		case $f in
		gldata.sto) [ -e $D/gldata.sto ] || mv $RAW/gldata.sto $D/gldata.sto; rm -f $RAW/gldata.sto ;;
		lto2.dat) mv -f $RAW/lto2.dat $D/lto2.dat ;;
		last) [ -e $LAST ] || mv $RAW/last $LAST; rm -f $RAW/last ;;
		esac
	done
	[ "$PREP" = cold ] && systemctl restart ztos-gnss-gate.service
	[ "${KEEP_RAW:-0}" = 1 ] || rm -f $RAW/$NAME.nmea
	chown -R 1000:1000 $H
	log "done: $H/$NAME.json"
	python3 - "$H/$NAME" <<'EOF'
import json, statistics, sys
n = sys.argv[1]
def cur(p):
    v = [l.split() for l in open(p) if l.strip()]
    ma = [int(x[0]) for x in v]
    mw = [int(x[0]) * int(x[1]) / 1e6 for x in v if len(x) > 1]
    return ma, mw
b, bw = cur(n + ".base")
g, gw = cur(n + ".gnss")
g60, gw60 = g[60:] or g, gw[60:] or gw
r = json.load(open(n + ".json"))
print("TTFF %s s; first engine accuracy %s s; accuracy median %s m p95 %s m; HDOP median %s; sats used (GGA) median %s max %s"
      % (r["ttff_s"], r["first_accuracy_s"], r["accuracy_m_median"], r["accuracy_m_p95"], r["hdop_median"],
         r["sats_used_gga_median"], r["sats_used_gga_max"]))
for s, v in sorted(r["systems"].items()):
    print("  %-8s view %s tracked %s used %s (max %s, %d epochs) SNR median %s max %s"
          % (s, v["in_view_median"], v["tracked_median"], v["used_median"], v["used_max"], v["epochs_used"], v["snr_median"], v["snr_max"]))
print("current: baseline median %d mA (n=%d, %d..%d); GNSS after 60 s median %d mA (n=%d, %d..%d); delta %d mA"
      % (statistics.median(b), len(b), min(b), max(b), statistics.median(g60), len(g60), min(g60), max(g60),
         statistics.median(g60) - statistics.median(b)))
if bw and gw60:
    print("power: baseline %.0f mW, GNSS %.0f mW, delta %.0f mW" % (statistics.median(bw), statistics.median(gw60), statistics.median(gw60) - statistics.median(bw)))
EOF
	;;
idle)
	SECS=${2:-60}
	lhd=$(pgrep -x lhd)
	cs() { awk '/ctxt_switches/ {s += $2} END {print s}' /proc/$lhd/task/*/status 2>/dev/null; }
	t() { awk '{print $14 + $15}' /proc/$lhd/stat; }
	l0=$(stat -c %Y /run/ztoscsf-sensors/light 2>/dev/null)
	c0=$(cs); t0=$(t); j0=$(date +%s)
	sleep "$SECS"
	c1=$(cs); t1=$(t)
	echo "unit $(systemctl is-active ztos-gnss.service); gpsd pids: $(pgrep -f /vendor/bin/hw/gpsd | tr '\n' ' ')"
	echo "lhd $lhd: $((t1 - t0)) CPU ticks, $((c1 - c0)) context switches in $SECS s"
	echo "lhd threads: $(for x in /proc/$lhd/task/*; do cat $x/wchan; echo; done | sort | uniq -c | tr '\n' ' ')"
	echo "hub ready $(cat /sys/class/sec/sensorhub/ready); light file updated $(( $(stat -c %Y /run/ztoscsf-sensors/light) - l0 )) s later than at the start"
	echo "inhibit: $(ls /run/zrn-sleepd/inhibit/)"
	echo "crash lines: $(journalctl --since @$j0 --no-pager -o cat | grep -c 'ESW:CRASHED\|MCU_REQ_RESP timeout\|line=0165\|hub reset')"
	;;
*)
	sed -n 2,26p "$0"; exit 2 ;;
esac
