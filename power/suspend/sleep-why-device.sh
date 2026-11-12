#!/bin/bash
# On the phone, as the session user, inside `ztlock run`: synthetic sleep
# blockers (an inhibit, a sound, a GPS reader through the gate), what
# zrn-sleepd status and zrn-sleep-why --shell say about each, and the test
# inhibit removed again whatever happens. ROADMAP 8.30.
#
#   power/suspend/sleep-why-device.sh [SOUND_FILE]
set -u
INH=/run/zrn-sleepd/inhibit/awake-why-test
SOUND=${1:-/usr/share/sounds/freedesktop/stereo/complete.oga}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
cleanup() {
	rm -f "$INH"
	[ -n "${GPS:-}" ] && kill "$GPS" 2>/dev/null
	[ -n "${PLAY:-}" ] && kill "$PLAY" 2>/dev/null
}
trap cleanup EXIT
show() {
	echo "== $1"
	sudo zrn-sleepd status | grep -E '^(why|blocker|woke_last_hour)='
	echo "-- as the shell sees it:"
	/usr/local/sbin/zrn-sleep-why --shell | grep -E '^(line|item)'
}

show "baseline"

printf 'label=Sleep-why test\npid=%s\n' $$ > "$INH"
show "an inhibit (pid=$$, label=)"

# a sound long enough to be seen: the file looped, quiet
( for i in 1 2 3 4 5 6; do paplay --volume=8000 "$SOUND"; done ) & PLAY=$!
sleep 3
show "a sound playing"
kill $PLAY 2>/dev/null; wait $PLAY 2>/dev/null; PLAY=

# a location reader through the gate: it starts the engine and holds timed-gnss
python3 -c 'import socket,time; s=socket.create_connection(("127.0.0.1",10110)); t=time.time()
while time.time()-t<40: s.recv(4096)' & GPS=$!
sleep 12
show "a GPS reader on 127.0.0.1:10110"
kill $GPS 2>/dev/null; wait $GPS 2>/dev/null; GPS=

rm -f "$INH"
sleep 1
show "after (test inhibit removed)"
ls /run/zrn-sleepd/inhibit/
