#!/bin/sh
# On the phone: ztos-bt-agent against tests/bt-agent-mock.py in place of bluetoothd.
#   tests/bt-agent-mock.sh start    the real agent stopped, the pretend one up
#   tests/bt-agent-mock.sh stop     all gone, the real agent back
# Commands go to /tmp/btmock.in; what comes back is in /tmp/btmock.out.
here=$(dirname "$(readlink -f "$0")")
conf=/tmp/btmock.conf
case "$1" in
start)
	systemctl --user stop ztos-bt-agent
	rm -f /tmp/btmock.in /tmp/btmock.out; mkfifo /tmp/btmock.in
	cat > $conf <<'EOF'
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path=/tmp/btmock.bus</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/>
    <allow own="*"/>
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
  </policy>
</busconfig>
EOF
	nohup dbus-daemon --config-file=$conf --nofork --nopidfile >/dev/null 2>&1 </dev/null &
	echo $! > /tmp/btmock.dbus.pid
	sleep 0.5
	addr=unix:path=/tmp/btmock.bus
	nohup sudo env DBUS_SESSION_BUS_ADDRESS=$addr python3 "$here/bt-agent-mock.py" /tmp/btmock.in >/tmp/btmock.out 2>&1 </dev/null &
	sleep 1
	DBUS_SYSTEM_BUS_ADDRESS=$addr nohup "${2:-/usr/local/bin/ztos-bt-agent}" >/tmp/btmock.agent.log 2>&1 </dev/null &
	echo $! > /tmp/btmock.agent.pid
	sleep 1.5
	;;
stop)
	echo quit > /tmp/btmock.in
	sleep 0.5
	kill "$(cat /tmp/btmock.agent.pid)" "$(cat /tmp/btmock.dbus.pid)" 2>/dev/null
	sleep 0.3
	rm -f /tmp/btmock.in /tmp/btmock.bus /tmp/btmock.conf /tmp/btmock.agent.pid /tmp/btmock.dbus.pid
	systemctl --user start ztos-bt-agent
	;;
esac
