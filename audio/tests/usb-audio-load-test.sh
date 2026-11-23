#!/bin/sh
# ztos-usb-audio-load (ROADMAP 8.62) on the host, with a pretend modprobe:
# it loads once, leaves no mark after a load that finished or failed cleanly,
# keeps the mark after a killed one, and then refuses until the mark goes.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
LOAD=$HERE/../../devices/samsung-starlte/overlay/usr/local/sbin/ztos-usb-audio-load
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin" "$T/sys"
export ZTOS_USB_AUDIO_MARK="$T/var/usb-loading" ZTOS_SYSMODULE="$T/sys" PATH="$T/bin:$PATH"
fails=0
check() {	# what, expected, got
	if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: expected '$2', got '$3'"; fails=$((fails + 1)); fi
}
fake() {	# the status the pretend modprobe ends with
	cat > "$T/bin/modprobe" <<EOF
#!/bin/sh
echo "\$*" >> "$T/calls"
[ -e "$ZTOS_USB_AUDIO_MARK" ] && echo marked >> "$T/calls"
case $1 in kill) kill -9 \$\$ ;; esac
exit $1
EOF
	chmod +x "$T/bin/modprobe"
	: > "$T/calls"
}

fake 0
sh "$LOAD" 2> /dev/null; check "a load" 0 $?
check "modprobe skips the install line, the mark is there while it runs" \
	"--ignore-install snd-usb-audio marked" "$(tr '\n' ' ' < "$T/calls" | sed 's/ $//')"
check "no mark after it" no "$([ -e "$ZTOS_USB_AUDIO_MARK" ] && echo yes || echo no)"

fake 1
sh "$LOAD" 2> /dev/null; check "a clean failure is passed on" 1 $?
check "and leaves no mark" no "$([ -e "$ZTOS_USB_AUDIO_MARK" ] && echo yes || echo no)"

fake kill
sh "$LOAD" 2> /dev/null; check "killed while loading (an oops)" 137 $?
check "keeps the mark" yes "$([ -e "$ZTOS_USB_AUDIO_MARK" ] && echo yes || echo no)"
fake 0
sh "$LOAD" 2> "$T/err"; check "with the mark: refused" 1 $?
check "and modprobe not run" "" "$(cat "$T/calls")"
grep -q "remove $ZTOS_USB_AUDIO_MARK" "$T/err"; check "says how to retry" 0 $?
rm "$ZTOS_USB_AUDIO_MARK"
sh "$LOAD" 2> /dev/null; check "mark removed: loads again" 0 $?

mkdir "$T/sys/snd_usb_audio"
touch "$ZTOS_USB_AUDIO_MARK"
fake 0
sh "$LOAD" 2> /dev/null; check "loaded already: nothing to do" 0 $?
check "and modprobe not run" "" "$(cat "$T/calls")"

[ $fails -eq 0 ] && echo "usb-audio-load: ok" || { echo "usb-audio-load: $fails failed"; exit 1; }
