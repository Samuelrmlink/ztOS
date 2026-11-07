#!/bin/sh
# ztnotifyd itself, with nothing of a running session in reach (make
# check-notify-daemon). It gets a bus of its own (dbus-run-session), a runtime
# directory of its own with a fifo standing in for the shell's control pipe, and
# a temporary $HOME with a fake Grayjay in it. Run under the name ztnotifyd-test,
# so that the shell's own pgrep/pkill for ztnotifyd can never find it.
set -eu
bin=$(realpath "$1")
tmp=$(mktemp -d /tmp/ztnotifyd-test.XXXXXX)
reader=
trap '[ -z "$reader" ] || kill "$reader" 2>/dev/null || true; rm -rf "$tmp"' EXIT
cp "$bin" "$tmp/ztnotifyd-test"
mkdir "$tmp/run" "$tmp/home"
mkfifo "$tmp/run/ztmetro.ctl"
grayjay="$tmp/home/.var/app/app.grayjay.Grayjay/data/Grayjay"
mkdir -p "$grayjay/downloads_ongoing" "$grayjay/downloads" "$grayjay/temp_files"
# a download left behind by an app that quit, its pieces with it
printf '{"State":0,"Video":{"ID":{"Value":"left-behind-id"},"Name":"Left behind","Duration":100},"VideoSource":{"Container":"application/vnd.apple.mpegurl","Bitrate":8000000},"Progress":0}' \
	>"$grayjay/downloads_ongoing/old-uuid"
head -c 200000 /dev/zero >"$grayjay/temp_files/segment-old"
touch -d '2 days ago' "$grayjay/downloads_ongoing/old-uuid" "$grayjay/temp_files/segment-old"

# keeps the fifo open, and everything said down it in a file
cat <>"$tmp/run/ztmetro.ctl" >"$tmp/said" &
reader=$!

cat >"$tmp/inner.sh" <<'EOF'
set -eu
XDG_RUNTIME_DIR="$TMPROOT/run" HOME="$TMPROOT/home" "$TMPROOT/ztnotifyd-test" 2>"$TMPROOT/log" &
daemon=$!
trap 'kill "$daemon" 2>/dev/null || true' EXIT
i=0
until gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
		-m org.freedesktop.DBus.GetNameOwner org.freedesktop.Notifications >/dev/null 2>&1; do
	i=$((i + 1))
	[ "$i" -lt 50 ] || { echo "the daemon never took the name"; cat "$TMPROOT/log"; exit 1; }
	sleep 0.1
done
# APP ID ICON SUMMARY BODY HINTS -> the id
notify() {
	gdbus call --session -d org.freedesktop.Notifications -o /org/freedesktop/Notifications \
		-m org.freedesktop.Notifications.Notify "$1" "$2" "$3" "$4" "$5" '[]' "$6" 5000 |
		sed 's/.*uint32 \([0-9]*\).*/\1/'
}
notify Mail 0 '' 'Plain one' 'hello | world' '{}' >/dev/null
id=$(notify curl 0 '' 'Fetching' '10%' '{"value": <int32 10>, "category": <"transfer">}')
[ "$(notify curl "$id" '' 'Fetching' '50%' '{"value": <uint32 50>}')" = "$id" ]
notify curl "$id" '' 'Fetching' 'all in' '{"value": <int32 100>}' >/dev/null
notify curl "$id" '' 'Fetched' 'done' '{}' >/dev/null
notify job 0 '' 'Working' 'no number' '{"category": <"progress">}' >/dev/null
notify job 0 '' 'Script' '1' '{"x-canonical-private-synchronous": <"my-bar">, "value": <int32 1>}' >/dev/null
gdbus call --session -d org.freedesktop.Notifications -o /org/freedesktop/Notifications \
	-m org.freedesktop.Notifications.CloseNotification "$id" >/dev/null
# a real download starting, in the fake home
printf '{"State":0,"Video":{"ID":{"Value":"abcdef-1234"},"Name":"A | video","Duration":100},"VideoSource":{"Container":"video/mp4","Bitrate":8000000},"Progress":0}' \
	>"$TMPROOT/home/.var/app/app.grayjay.Grayjay/data/Grayjay/downloads_ongoing/new-uuid"
sleep 2
head -c 40000000 /dev/zero >"$TMPROOT/home/.var/app/app.grayjay.Grayjay/data/Grayjay/downloads/abcdef-1234 [1x1].mp4"
sleep 4
rm "$TMPROOT/home/.var/app/app.grayjay.Grayjay/data/Grayjay/downloads_ongoing/new-uuid"
sleep 2
# how little it does at rest: nothing shown, a download's file still waiting
stat=/proc/$daemon/stat
before=$(awk '{print $14 + $15}' "$stat")
ctx0=$(awk '/^voluntary/ {print $2}' /proc/$daemon/status)
sleep 10
after=$(awk '{print $14 + $15}' "$stat")
ctx1=$(awk '/^voluntary/ {print $2}' /proc/$daemon/status)
echo "$((after - before)) ticks, $((ctx1 - ctx0)) wakes in 10 s at rest" >"$TMPROOT/rest"
EOF
TMPROOT="$tmp" dbus-run-session -- sh "$tmp/inner.sh"
sleep 0.2

said="$tmp/said"
fail=0
want() {
	if ! grep -qF -- "$1" "$said"; then
		echo "FAIL: nothing said matching: $1"
		fail=1
	fi
}
refuse() {
	if grep -qF -- "$1" "$said"; then
		echo "FAIL: should not have said: $1"
		fail=1
	fi
}
want '|-|-1||Mail|Plain one|hello / world'
want '|q|10||Curl|Fetching|10%'
want '|q|50||Curl|Fetching|50%'
want '|q|-2||Curl|Fetching|all in'
# (no longer progress, but quiet: a quick update of the same id doesn't alert)
want '|q|-1||Curl|Fetched|done'
want '|q|-2||Job|Working|no number'
want 'notekey sync:my-bar|q|1||Job|Script|1'
want 'noteclose bus:'
want 'notekey dl:grayjay:new-uuid|q|-2|app.grayjay.Grayjay|Grayjay|A / video|Starting'
# (8 Mbit/s for 100 s is 100 MB, and 40 MB of it is on disk)
want 'notekey dl:grayjay:new-uuid|q|39|app.grayjay.Grayjay|Grayjay|A / video|About 39% (40 MB)'
want 'noteclose dl:grayjay:new-uuid'
refuse 'Left behind'
refuse 'old-uuid'
curl_keys=$(grep -F '|Curl|' "$said" | cut -d'|' -f1 | sort -u | wc -l)
[ "$curl_keys" = 1 ] || { echo "FAIL: one notification became $curl_keys"; fail=1; }
cat "$tmp/rest"
rest_wakes=$(sed 's/.*, \([0-9]*\) wakes.*/\1/' "$tmp/rest")
[ "$rest_wakes" -le 2 ] || { echo "FAIL: it is not resting"; fail=1; }
if [ "$fail" != 0 ]; then
	echo "--- said"
	cat "$said"
	echo "--- log"
	cat "$tmp/log"
	exit 1
fi
echo "PASS: ztnotifyd on a private bus: hints, ids, closing, and a fake Grayjay"
