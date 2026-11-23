#!/bin/bash
# On the phone, as the session user, inside `ztlock run`: does the lock
# screen's awake line cost anything with the screen off? A/B over SECONDS
# each, lock_awake on then off (config + SIGHUP), the screen off throughout:
# the shell's context switches (all threads), the system's, interrupts, and
# any zrn-sleep-why started. The config is put back. ROADMAP 8.30.
#
#   power/suspend/sleep-why-cost.sh [SECONDS]
set -u
SECS=${1:-60}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
CTL=$XDG_RUNTIME_DIR/ztmetro.ctl
CONF=$HOME/.config/ztmetro/config
SHELL_PID=$(pgrep -x -u "$(id -u)" ztmetro | head -1)
cp -a "$CONF" "$CONF.sleep-why-cost"
restore() { cp -a "$CONF.sleep-why-cost" "$CONF"; rm -f "$CONF.sleep-why-cost"; kill -HUP "$SHELL_PID"; }
trap restore EXIT

switches() { cat /proc/"$SHELL_PID"/task/*/status | awk '/ctxt_switches/ {s += $2} END {print s}'; }
sys() { awk '/^ctxt/ {c=$2} /^intr/ {i=$2} END {print c, i}' /proc/stat; }

run() {
	local mode=$1
	sed -i '/^lock_awake *=/d' "$CONF"
	echo "lock_awake = $mode" >> "$CONF"
	kill -HUP "$SHELL_PID"
	echo "screen on" > "$CTL"; sleep 4		# the lock screen lit: one look
	echo "screen off" > "$CTL"; sleep 10	# settle
	local s0 c0 i0 s1 c1 i1 spawned=0
	s0=$(switches); read -r c0 i0 < <(sys)
	for ((t = 0; t < SECS; t += 2)); do
		pgrep -f zrn-sleep-why > /dev/null && spawned=$((spawned + 1))
		sleep 2
	done
	s1=$(switches); read -r c1 i1 < <(sys)
	echo "lock_awake=$mode screen off ${SECS}s: ztmetro switches $((s1 - s0)), system ctxt $((c1 - c0)), irqs $((i1 - i0)), zrn-sleep-why seen in $spawned of $((SECS / 2)) looks"
}
grep -q '^screen off' "$XDG_RUNTIME_DIR/ztmetro.state" && echo "(was off)"
run on
run off
run on
echo "screen on" > "$CTL"
