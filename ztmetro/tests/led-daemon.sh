#!/bin/sh
# make check-led: zrn-ledd itself, against a fake sysfs directory. Its claims
# survive its own death, a light is written only when it changes, and it does
# not wake by itself.
BIN=$(readlink -f "$1")
T=$(mktemp -d)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$T"' EXIT
mkdir -p "$T/run"
for c in red green blue; do
	mkdir -p "$T/sys/$c:status"
	for a in brightness trigger ramp delay_on delay_off; do : > "$T/sys/$c:status/$a"; done
done
CONFIG=$T/config
: > "$CONFIG"

fail() { echo "led-daemon: FAIL: $*"; cat "$T/log" 2>/dev/null; exit 1; }
start() { "$BIN" --state "$T/run" --sys "$T/sys" --config "$CONFIG" >> "$T/log" 2>&1 & PID=$!; }
die() { kill -9 $PID; wait $PID 2>/dev/null; PID=; }
b() { cat "$T/sys/$1:status/brightness"; }
now() { echo "$(b red) $(b green) $(b blue)"; }
# the light should come to say this; a moment is allowed
expect() {
	i=0
	while [ "$(now)" != "$1" ]; do
		i=$((i + 1))
		[ $i -gt 100 ] && fail "$2: light is '$(now)', wanted '$1'"
		sleep 0.02
	done
}
trig() { [ "$(cat "$T/sys/$1:status/trigger")" = "$2" ] || fail "$3: $1 trigger is $(cat "$T/sys/$1:status/trigger"), wanted $2"; }
attr() { [ "$(cat "$T/sys/$1")" = "$2" ] || fail "$3: $1 is $(cat "$T/sys/$1"), wanted $2"; }
settle() { sleep 0.15; }
poke() { : > "$T/run/user/settings"; }

# anything left on the light goes at once, with nothing claimed
echo 200 > "$T/sys/red:status/brightness"; echo timer > "$T/sys/red:status/trigger"
start
expect "0 0 0" "nothing claimed"
trig red none "nothing claimed"
[ -d "$T/run/user" ] || fail "no user directory"

echo charging > "$T/run/charge"; expect "24 0 0" "charging"
settle; trig red none "charging"; attr "red:status/ramp" "0 0" "charging"
echo full > "$T/run/charge"; expect "0 24 0" "full"
echo none > "$T/run/charge"; expect "0 0 0" "unplugged"

echo low > "$T/run/charge"; expect "120 0 0" "low"
settle
trig red timer "low"; trig green none "low"
attr "red:status/ramp" "400 400" "low"; attr "red:status/delay_on" 500 "low"; attr "red:status/delay_off" 500 "low"

# a notification over low battery, and back when it clears
echo 1 > "$T/run/user/notify"; expect "0 0 120" "notification"
settle
trig blue timer "notification"; trig red none "notification"
attr "red:status/ramp" "600 600" "notification"; attr "blue:status/delay_on" 750 "notification"
echo 0 > "$T/run/user/notify"; expect "120 0 0" "cleared"
settle; trig blue none "cleared"; trig red timer "cleared"

# RapidBoost: off by default, whatever zrnperfd says
echo none > "$T/run/charge"; expect "0 0 0" "unplugged"
echo 1 > "$T/run/boost"; settle
expect "0 0 0" "boost, default"
echo "led_boost = on" > "$CONFIG"; poke; expect "40 40 40" "boost, asked for"
settle; trig green timer "boost"; attr "red:status/ramp" "0 0" "boost"
# the lowest: a charger takes the light, and gives it back
echo charging > "$T/run/charge"; expect "24 0 0" "charging over boost"
echo none > "$T/run/charge"; expect "40 40 40" "boost again"
echo 0 > "$T/run/boost"; expect "0 0 0" "boost done"

# the same claim again writes nothing: a blink would restart
echo charging > "$T/run/charge"; expect "24 0 0" "charging"
settle
echo 99 > "$T/sys/red:status/brightness"
echo charging > "$T/run/charge"; settle
echo 1 > "$T/run/boost"; settle		# (under a charger: no change to show)
[ "$(b red)" = 99 ] || fail "wrote the light with nothing changed"
echo 0 > "$T/run/boost"
# SIGHUP: written again, as udev asks when the light appears
kill -HUP $PID; expect "24 0 0" "SIGHUP"

# settings: colours, a source switched off, the master
printf 'led_charging_colour = 00ff00\n' > "$CONFIG"; poke; expect "0 24 0" "colour"
printf 'led_charging = off\n' > "$CONFIG"; poke; expect "0 0 0" "switched off"
printf 'other=1\nled = off\nled_charging = on\n' > "$CONFIG"; poke; expect "0 0 0" "master off"
echo 1 > "$T/run/user/notify"; settle; expect "0 0 0" "master off, notification"
: > "$CONFIG"; poke; expect "0 0 120" "defaults again"
echo 0 > "$T/run/user/notify"; expect "24 0 0" "cleared"

# its own death: what is claimed comes back, and what changed meanwhile
die
echo low > "$T/run/charge"		# the charger came out while it was down
echo 1 > "$T/run/boost"
echo 99 > "$T/sys/red:status/brightness"
start
expect "120 0 0" "restart: low"
settle; trig red timer "restart"
die; echo charging > "$T/run/charge"; echo 1 > "$T/run/user/notify"; start
expect "0 0 120" "restart: a notification still pending"
echo 0 > "$T/run/user/notify"; expect "24 0 0" "restart: cleared"
echo 0 > "$T/run/boost"

# half a write is not a claim: empty keeps what was said until the close
: > "$T/run/charge"; settle
expect "24 0 0" "empty claim"
echo charging > "$T/run/charge"
# a removed one is
rm "$T/run/charge"; expect "0 0 0" "claim removed"
echo charging > "$T/run/charge"; expect "24 0 0" "charging"

# the session's directory: a link or a pipe is no claim, and hangs nothing
ln -sf "$T/run/charge" "$T/run/user/notify"; : > "$T/run/user/x"; settle
expect "24 0 0" "link"
rm "$T/run/user/notify"; mkfifo "$T/run/user/notify"; echo full > "$T/run/charge"
expect "0 24 0" "pipe"
rm "$T/run/user/notify"

# no light yet: nothing breaks, and SIGHUP writes it when there is one
die
mv "$T/sys" "$T/sys.away"
echo charging > "$T/run/charge"
start; settle
kill -0 $PID 2>/dev/null || fail "died without a light"
mv "$T/sys.away" "$T/sys"
echo 0 > "$T/sys/red:status/brightness"
kill -HUP $PID; expect "24 0 0" "light appeared"

# it does not wake by itself
settle
wakes() { awk '/^voluntary_ctxt/ { v = $2 } /^nonvoluntary_ctxt/ { n = $2 } END { print v + n }' "/proc/$PID/status"; }
a=$(wakes); sleep 3; c=$(wakes)
[ "$a" = "$c" ] || fail "woke $((c - a)) times in 3 s with nothing to do"
kb=$(awk '/^VmRSS/ { print $2 }' "/proc/$PID/status")
echo "led-daemon: ok (0 wakes in 3 s idle, ${kb} kB resident)"
