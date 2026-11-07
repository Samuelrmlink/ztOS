#!/bin/bash
# scripts/flash-root.sh against a pretend phone: a scratch file for the partition
# and a stand-in for psh.py that runs the phone's side on this computer, over
# loopback. No phone, no block device. Each case has a time limit, because the
# failure being tested is a hang.
#
#   scripts/tests/test-flash-root.sh
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
fails=0

# the "phone": PHONE_MODE=ok runs each command, =blocked refuses the connection
# as a firewall would; =silent lets the probe through, then blocks the transfer
cat > "$W/psh" <<'EOF'
#!/bin/bash
[ "$1" = -t ] && shift 2
cmd=${*//\/dev\/sda25/$FAKE_PART}
case $cmd in
*killall*|touch\ /run/*) exit 0 ;;
cat\ /sys/class/block/*) echo PARTNAME=USERDATA; exit 0 ;;
blockdev*) stat -c %s "$FAKE_PART"; exit 0 ;;
"ps | "*) pgrep -f "dd of=$FAKE_PART" > /dev/null && echo running; exit 0 ;;
esac
case $PHONE_MODE:$cmd in
blocked:*nc\ *) exit 1 ;;
silent:setsid*) exit 0 ;;
esac
exec bash -c "$cmd"
EOF
chmod +x "$W/psh"

run() {		# NAME MODE WANT_STATUS WANT_TEXT
	local name=$1 mode=$2 want=$3 text=$4 out status
	out=$(PHONE_MODE=$mode FAKE_PART=$W/part PSH=$W/psh HOST=127.0.0.1 PORT=$((5900 + RANDOM % 90)) DRAIN_WAIT=3 \
		timeout 90 "$ROOT/scripts/flash-root.sh" "$W/img" 1 2>&1)
	status=$?
	if [ "$status" = "$want" ] && grep -q "$text" <<< "$out"; then
		echo "ok    $name"
	else
		echo "FAIL  $name (status $status, wanted $want and \"$text\")"
		sed 's/^/      /' <<< "$out"
		fails=$((fails + 1))
	fi
}

head -c 3145728 /dev/urandom > "$W/img"
truncate -s 4M "$W/part"

run "firewall: stops before writing, says so" blocked 2 "could not connect"
cmp -s -n 1048576 "$W/part" /dev/zero || { echo "FAIL  the blocked run wrote to the partition"; fails=$((fails + 1)); }

run "blocked after the check: no hang" silent 2 "could not connect"

run "a clean run writes and verifies every chunk" ok 0 "FLASH_ROOT_OK 3 chunk"
cmp -s -n 3145728 "$W/img" "$W/part" || { echo "FAIL  the partition differs from the image"; fails=$((fails + 1)); }

run "a second run sends nothing" ok 0 "FLASH_ROOT_OK 0 chunk(s) written, 3 already identical"

# nothing can listen on an address this computer does not have
out=$(PHONE_MODE=ok FAKE_PART=$W/part PSH=$W/psh HOST=192.0.2.1 PORT=5999 timeout 30 "$ROOT/scripts/flash-root.sh" "$W/img" 1 2>&1)
status=$?
if [ "$status" = 1 ] && grep -q "could not listen" <<< "$out"; then
	echo "ok    wrong address: says it cannot listen"
else
	echo "FAIL  wrong address (status $status)"; sed 's/^/      /' <<< "$out"; fails=$((fails + 1))
fi

[ "$fails" = 0 ] && echo "all passed" || { echo "$fails failed"; exit 1; }
