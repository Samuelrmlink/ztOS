#!/bin/bash
# sleep-alarm-test.sh: deep sleep cycles and an alarmtimer wake, unattended.
# Run as root on the phone, detached (ssh goes down while it sleeps):
#   sudo setsid -f bash sleep-alarm-test.sh [RTC_S] [ALARM_S] > /dev/null 2>&1
# Log: /var/tmp/sleep-alarm-test.log; battery rows: /var/lib/zrn-sleep/soc.csv.
#  1. zrn-sleepd once RTC_S (60): woken by its own RTC alarm
#  2. a CLOCK_REALTIME_ALARM timerfd ALARM_S (90) out, then once 300: woken
#     by that alarm, which must fire on time (alarm-timerfd.py logs lateness)
#  3. once RTC_S again
# After each: how long Wi-Fi takes to be up again.
RTC_S=${1:-60}
ALARM_S=${2:-90}
LOG=/var/tmp/sleep-alarm-test.log
HERE=$(dirname "$(readlink -f "$0")")
ALARM=$HERE/alarm-timerfd.py
[ -f "$ALARM" ] || ALARM=/var/tmp/alarm-timerfd.py
say() { echo "$(date '+%F %T') $*" >> $LOG; }
wifi_wait() {
	local t0=$(date +%s.%N) s
	for _ in $(seq 1 120); do
		s=$(cat /sys/class/net/wlp1s0/operstate 2>/dev/null)
		if [ "$s" = up ]; then
			say "wifi up after $(awk -v a="$(date +%s.%N)" -v b="$t0" 'BEGIN{printf "%.1f", a-b}') s ($(iw dev wlp1s0 link | awk '/SSID/{print $2}'))"
			return
		fi
		sleep 0.25
	done
	say "wifi not up after 30 s (${s:-absent})"
}
say "start: battery $(cat /sys/class/power_supply/max170xx_battery/capacity)%"
zrn-sleepd once "$RTC_S" >> $LOG 2>&1
wifi_wait
sleep 5
python3 "$ALARM" "$ALARM_S" $LOG > /dev/null 2>&1 &
sleep 1
zrn-sleepd once 300 >> $LOG 2>&1
wifi_wait
wait
sleep 5
zrn-sleepd once "$RTC_S" >> $LOG 2>&1
wifi_wait
say "done: battery $(cat /sys/class/power_supply/max170xx_battery/capacity)%"
