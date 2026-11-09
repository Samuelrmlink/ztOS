#!/bin/bash
# ztacctd's CPU power table against the battery gauge, for the overnight queue:
#	sudo ztbench add acct-cpu /path/to/bench-cpu.sh 40
# Screen off, untouched, on battery. Phases of PHASE_S seconds (the first
# SETTLE_S not counted): idle; the four little cores busy; one big core busy;
# with zrnperfd's "deep sleep" (clusters held at their lowest clocks) as it is,
# then off (schedutil free to go up), then put back. Each phase: the gauge's
# coulomb counter and voltage, each cluster's time_in_state and busy time.
# $OUT/phases.csv is what fitting needs: watts above idle per busy core at the
# clocks the cluster ran at.
set -u
OUT=${OUT:-/tmp/acct-bench}
mkdir -p "$OUT"
PHASE_S=${PHASE_S:-300}
SETTLE_S=${SETTLE_S:-30}
B=/sys/class/power_supply/max170xx_battery
CTL=/run/zrnperfd.ctl
was_deep=$(sed -n 's/^deepsleep=//p' /run/zrnperfd.status)
pids=()

restore() {
	for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
	[ -n "$was_deep" ] && [ -p $CTL ] && echo "deepsleep $was_deep" > $CTL
}
trap restore EXIT

grep -q '^screen=off' /run/zrnperfd.status || { echo "the screen is on"; exit 1; }
[ "$(cat $B/status)" = Discharging ] || { echo "not on battery"; exit 1; }

busy_s() {	# busy jiffies of CPUs $1..$2
	awk -v a=$1 -v b=$2 '/^cpu[0-9]/ { n = substr($1, 4) + 0; if (n >= a && n <= b) s += $2 + $3 + $4 + $7 + $8 + $9 } END { print s + 0 }' /proc/stat
}
tis() {		# freq:time pairs of a policy
	awk '{ printf "%s:%s ", $1, $2 }' /sys/devices/system/cpu/cpufreq/policy$1/stats/time_in_state
}

echo "phase,deepsleep,seconds,uah_used,volts,watts,little_busy_s,big_busy_s,little_tis,big_tis" > "$OUT/phases.csv"
phase() {	# NAME DEEPSLEEP CPUS_TO_LOAD
	local name=$1 deep=$2 load=$3
	echo "deepsleep $deep" > $CTL
	for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
	pids=()
	for c in $load; do
		taskset -c "$c" sh -c 'while :; do :; done' &
		pids+=($!)
	done
	sleep "$SETTLE_S"
	local q0=$(cat $B/charge_counter) t0=$(date +%s.%N) l0=$(busy_s 0 3) b0=$(busy_s 4 7)
	local tl0=$(tis 0) tb0=$(tis 4) vs=0 n=0
	local end=$(( $(date +%s) + PHASE_S - SETTLE_S ))
	while [ "$(date +%s)" -lt $end ]; do
		vs=$(( vs + $(cat $B/voltage_now) )); n=$((n + 1))
		grep -q '^screen=off' /run/zrnperfd.status || { echo "screen came on"; exit 1; }
		sleep 5
	done
	local q1=$(cat $B/charge_counter) t1=$(date +%s.%N) l1=$(busy_s 0 3) b1=$(busy_s 4 7)
	local tl1=$(tis 0) tb1=$(tis 4)
	awk -v name=$name -v deep=$deep -v q0=$q0 -v q1=$q1 -v t0=$t0 -v t1=$t1 -v vs=$vs -v n=$n \
		-v l0=$l0 -v l1=$l1 -v b0=$b0 -v b1=$b1 -v tl0="$tl0" -v tl1="$tl1" -v tb0="$tb0" -v tb1="$tb1" '
	function delta(a, b,    i, n, m, x, y, s, out) {	# "f:t ..." twice: seconds at each clock
		n = split(a, x, " "); m = split(b, y, " "); out = ""
		for (i = 1; i <= m; i++) {
			split(x[i], p, ":"); split(y[i], q, ":")
			s = (q[2] - p[2]) / 100
			if (s > 0) out = out q[1] ":" s " "
		}
		return out
	}
	BEGIN {
		dt = t1 - t0; uah = q0 - q1; v = vs / n / 1e6
		w = uah * 3.6e-3 * v / dt
		printf "%s,%s,%.1f,%d,%.3f,%.3f,%.1f,%.1f,\"%s\",\"%s\"\n", name, deep, dt, uah, v, w,
			(l1 - l0) / 100, (b1 - b0) / 100, delta(tl0, tl1), delta(tb0, tb1)
	}' >> "$OUT/phases.csv"
	echo "$(date +%T) $name done"
}

online_big=$(for c in 4 5 6 7; do [ "$(cat /sys/devices/system/cpu/cpu$c/online 2>/dev/null)" = 1 ] && echo $c; done | head -1)
phase idle-deep on ""
phase little4-deep on "0 1 2 3"
[ -n "$online_big" ] && phase big1-deep on "$online_big"
phase idle off ""
phase little4 off "0 1 2 3"
online_big=$(for c in 4 5 6 7; do [ "$(cat /sys/devices/system/cpu/cpu$c/online 2>/dev/null)" = 1 ] && echo $c; done | head -1)
[ -n "$online_big" ] && phase big1 off "$online_big"
phase idle-end off ""
cat "$OUT/phases.csv"
