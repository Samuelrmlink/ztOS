#!/bin/sh
# ab.sh FIX ROUNDS: screen-off A/B drain test for a ROADMAP 8.31 fix (root,
# inside ztlock). FIX is mif (memory 1794 vs 421 MHz) or abox (audio block
# kept on vs off when idle). Each round measures A (fix off) then B (fix on),
# SETTLE s after the change, N one-second samples of current_now and
# voltage_now. AOD is turned off for the run (sleep inhibited, so the phone
# stays awake with the panel dark) and everything is put back on exit.
# On the charger the gauge sees the charge flow: the run says so, and only
# the A-B difference means anything.
FIX=${1:?mif or abox}; ROUNDS=${2:-2}
SETTLE=${SETTLE:-60} N=${N:-60}
B=/sys/class/power_supply/max170xx_battery
CTL=/run/user/1000/ztmetro.ctl
PERF=/run/zrnperfd.ctl
MIF=/sys/module/zrn_rails/parameters/mif_khz
IDLE=/sys/module/snd_zrn_abox/parameters/idle_off_ms
AOD_WAS=$(sed -n 's/^aod *= *//p' /home/samuel/.config/ztmetro/config 2>/dev/null)
AOD_WAS=${AOD_WAS:-off}
IDLE_WAS=$(cat $IDLE 2>/dev/null)
CHG=$(cat /sys/class/power_supply/max77705-charger/online)

undo() {
	case $FIX in
	mif) echo "mif-scale on" > $PERF ;;	# zrnperfd sets the level for the screen
	abox) [ -n "$IDLE_WAS" ] && echo "$IDLE_WAS" > $IDLE ;;
	esac
	echo "aod $AOD_WAS" > $CTL
	rm -f /run/zrn-sleepd/inhibit/rail-map
}
trap 'undo; echo "$(date +%T) undone (aod $AOD_WAS)"' EXIT
trap 'exit 1' INT TERM HUP

set_a() {	# the fix off
	case $FIX in
	mif) echo "mif-scale off" > $PERF; sleep 1; echo 1794000 > $MIF ;;
	abox) echo 0 > $IDLE ;;
	esac
}
set_b() {	# the fix on
	case $FIX in
	mif) echo "mif-scale off" > $PERF; sleep 1; echo 421000 > $MIF ;;
	abox) echo 500 > $IDLE ;;
	esac
}
irqs() { awk '/abox/{s=0;for(i=2;i<=NF;i++){if($i!~/^[0-9]+$/)break;s+=$i}; print s}' /proc/interrupts | head -1; }
meas() {	# LABEL
	sleep "$SETTLE"
	i0=$(irqs); s0=$(head -1 /proc/stat)
	i=0; : > /tmp/ab.$$
	while [ $i -lt $N ]; do echo "$(cat $B/current_now) $(cat $B/voltage_now)" >> /tmp/ab.$$; i=$((i+1)); sleep 1; done
	i1=$(irqs); s1=$(head -1 /proc/stat); avg=$(cat $B/current_avg)
	busy=$(printf '%s\n%s\n' "$s0" "$s1" | awk '{t=0;for(i=2;i<=NF;i++)t+=$i; id=$5+$6; if(NR==1){t0=t;i0=id}else printf "%.1f", 100*(1-(id-i0)/(t-t0))}')
	awk -v L="$1" -v avg="$avg" -v busy="$busy" -v ir="$(( (${i1:-0} - ${i0:-0}) / N ))" -v n="$N" '
	{ i=$1/1000; v=$2/1e6; s+=i; ss+=i*i; p+=i*v; k++ }
	END { m=s/k; sd=sqrt(ss/k-m*m); "date +%T" | getline now;
	 printf "%s %-6s n=%d current=%.1fmA sd=%.1f se=%.1f P=%.0fmW avg=%dmA busy=%s%% abox_irq=%d/s\n",
		now, L, k, m, sd, sd/sqrt(k), p/k, avg/1000, busy, ir }' /tmp/ab.$$
	rm -f /tmp/ab.$$
}

mkdir -p /run/zrn-sleepd/inhibit && touch /run/zrn-sleepd/inhibit/rail-map
echo "aod off" > $CTL; sleep 2
grep -q '^screen off' /run/user/1000/ztmetro.state || { echo "screen off" > $CTL; sleep 3; }
echo "$(date +%T) $FIX: charger=$CHG aod_was=$AOD_WAS settle=$SETTLE n=$N rounds=$ROUNDS (current_now sign as the gauge reports it)"
r=0
while [ $r -lt "$ROUNDS" ]; do
	set_a; meas A
	set_b; meas B
	r=$((r+1))
done
