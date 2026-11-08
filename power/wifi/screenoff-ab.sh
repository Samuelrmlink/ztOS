#!/bin/bash
# Screen-off Wi-Fi power A/B, run as root, detached (systemd-run). Waits for the
# screen to have been off for 60 s, publishes a schedule, then runs each phase:
# SETTLE s settling, MEAS s sampling (battery current_now, ISYS), PROBE s for the
# PC's reachability probes (probe.sh, started on the PC beforehand). Everything
# is restored at exit, whatever happens. Phases: A default, B power save off,
# C radio off, D DTIM skip + 200 ms sleep return, E D + packet filter, F filter
# only, G E + no bgscan. zrnperfd's own screen-off settings are held off meanwhile.
#   sudo systemd-run --unit=wifi-ab bash screenoff-ab.sh
D=${D:-/home/samuel/wifipm}	# wl.py lives here, the results go here
W="python3 $D/wl.py"
IF=wlp1s0
SETTLE=${SETTLE:-30}; MEAS=${MEAS:-150}; PROBE=${PROBE:-45}
PHASES=${PHASES:-"A E C B D A"}
OUT=$D/meas-$(date +%m%d-%H%M).csv
SCHED=$D/schedule
LOG=$D/meas.log
BAT=/sys/class/power_supply/max170xx_battery/current_now
for h in /sys/class/hwmon/hwmon*; do [ "$(cat $h/name)" = max77705 ] && ISYS=$h/curr2_input; done

defaults() {
	nmcli radio wifi on
	iw dev $IF set power_save on
	$W set pkt_filter_enable 100 0; $W set pkt_filter_delete 100
	$W set pkt_filter_enable 101 0; $W set pkt_filter_delete 101
	$W set pkt_filter_enable 102 0; $W set pkt_filter_delete 102
	$W set pkt_filter_enable 103 0; $W set pkt_filter_delete 103
	wpa_cli -i $IF set_network 0 bgscan '"simple:30:-65:300"'
	$W set bcn_li_dtim 0
	$W set pm2_sleep_ret 2000
}
filters() {
	# 100: unicast destination; 101: IPv6 router advertisements; 102: DHCP to port 68
	$W setb pkt_filter_add 64000000000000000000000000000000010000000100
	$W setb pkt_filter_add 650000000000000000000000""0c0000002b000000"$(python3 -c "
m=bytearray(43);p=bytearray(43)
for i,v in ((0,0x86),(1,0xdd),(8,0x3a),(42,0x86)): m[i]=0xff;p[i]=v
print((m+p).hex())")"
	$W setb pkt_filter_add 660000000000000000000000""0c0000001a000000"$(python3 -c "
m=bytearray(26);p=bytearray(26)
for i,v in ((0,8),(1,0),(11,17),(24,0),(25,68)): m[i]=0xff;p[i]=v
print((m+p).hex())")"
	$W setb pkt_filter_add 670000000000000000000000""0c0000002b000000"$(python3 -c "
m=bytearray(43);p=bytearray(43)
for i,v in ((0,0x86),(1,0xdd),(8,0x3a),(42,0x87)): m[i]=0xff;p[i]=v
print((m+p).hex())")"
	$W set pkt_filter_mode 1
	$W set pkt_filter_enable 100 1; $W set pkt_filter_enable 101 1; $W set pkt_filter_enable 102 1; $W set pkt_filter_enable 103 1
}
apply() {
	defaults >/dev/null 2>&1
	case $1 in
	A) ;;
	B) iw dev $IF set power_save off ;;
	C) nmcli radio wifi off ;;
	D) $W set pm2_sleep_ret 200; $W set bcn_li_dtim 2 ;;
	E) $W set pm2_sleep_ret 200; $W set bcn_li_dtim 2; filters ;;
	F) filters ;;
	G) $W set pm2_sleep_ret 200; $W set bcn_li_dtim 2; filters; wpa_cli -i $IF set_network 0 bgscan '""' ;;
	esac >> $LOG 2>&1
}
trap 'rm -f $SCHED' EXIT	# nothing changed before the lock
trap 'exit 1' TERM INT

screen_off() { grep -q '^screen=off' /run/zrnperfd.status; }
echo "$(date +%T) waiting for the screen to be off" > $LOG
# the shared phone lock (ztlock), held from setup to restore; the screen must
# have been off for 60 s, checked again once the lock is ours
exec 9</run/ztbench/lock	# read: root may not open samuel's file to write (protected_regular)
while :; do
	off=0
	while [ $off -lt 6 ]; do
		if screen_off; then off=$((off + 1)); else off=0; fi
		sleep 10
	done
	flock -w 1800 9 || continue
	if screen_off; then break; fi
	flock -u 9
done
echo "wifi-agent since $(date +%T): screen-off Wi-Fi power measurement (meas.sh, ~$(( $(echo $PHASES | wc -w) * (SETTLE + MEAS + PROBE) / 60 + 1)) min)" | sudo -u samuel tee /run/ztbench/owner >/dev/null
trap 'defaults >/dev/null 2>&1; echo "wifi-suspend on" > /run/zrnperfd.ctl; pkill -HUP -x ztmetro; echo "$(date +%T) restored, AOD back to the config" >> $LOG; rm -f $SCHED /run/ztbench/owner' EXIT
# a dark screen: the AOD off for this session only (SIGHUP to ztmetro rereads the config)
echo 'wifi-suspend off' > /run/zrnperfd.ctl	# zrnperfd's own screen-off settings out of the way
timeout 5 sh -c "echo 'aod off' > /run/user/1000/ztmetro.ctl"; echo "$(date +%T) aod off" >> $LOG
T0=$(( $(date +%s) + 60 ))
PH=$((SETTLE + MEAS + PROBE))
: > $SCHED.new
i=0
for p in $PHASES; do
	s=$((T0 + i * PH))
	echo "$p $s $((s + SETTLE)) $((s + SETTLE + MEAS)) $((s + PH))" >> $SCHED.new
	i=$((i + 1))
done
mv $SCHED.new $SCHED
echo "$(date +%T) schedule T0=$T0" >> $LOG
echo "phase,t,bat_ua,isys_ma,screen" > $OUT
while read -r p s m e end; do
	while [ $(date +%s) -lt $s ]; do sleep 1; done
	echo "$(date +%T) phase $p" >> $LOG
	apply $p
	echo "$(date +%T) phase $p: $(iw dev $IF get power_save 2>&1) dtim=$($W get bcn_li_dtim 8 1 2>&1) ret=$($W get pm2_sleep_ret 8 1 2>&1)" >> $LOG
	while [ $(date +%s) -lt $m ]; do sleep 1; done
	while [ $(date +%s) -lt $e ]; do
		read b < $BAT; read y < $ISYS
		if screen_off; then sc=0; else sc=1; fi
		echo "$p,$(date +%s),$b,$y,$sc" >> $OUT
		sleep 2
	done
	# the radio-off phase gives the network back for the probe window
	[ $p = C ] && nmcli radio wifi on >> $LOG 2>&1
	if [ $p = E ] || [ $p = F ] || [ $p = G ]; then
		echo "$(date +%T) filter: $($W getp pkt_filter_stats 100 16 2>&1)" >> $LOG
	fi
	while [ $(date +%s) -lt $end ]; do sleep 1; done
done < $SCHED
echo "$(date +%T) done $OUT" >> $LOG
