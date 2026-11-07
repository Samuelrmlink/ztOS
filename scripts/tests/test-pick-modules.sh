#!/bin/bash
# devices/samsung-starlte/pick-modules.sh, on a scratch tree, and then on this
# repository's real prebuilts. No kernel and no phone.
#
#   scripts/tests/test-pick-modules.sh
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
PICK=$ROOT/devices/samsung-starlte/pick-modules.sh
fails=0
ok() { echo "ok    $1"; }
bad() { echo "FAIL  $1"; [ -z "${2:-}" ] || sed 's/^/      /' <<< "$2"; fails=$((fails + 1)); }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/prebuilts/modules" "$T/leds"
ko() { printf 'x\0vermagic=%s SMP preempt\0' "${2:-7.1.0-zboot}" > "$1"; }
old() { touch -d '2026-01-01 00:00' "$1"; }
new() { touch -d '2026-06-01 00:00' "$1"; }
ko "$T/prebuilts/modules/zrn-flash.ko"
ko "$T/prebuilts/modules/zrn-led.ko"
echo 'the source' > "$T/leds/zrn-flash.c"
(cd "$T" && sha256sum leds/zrn-flash.c > prebuilts/modules/SOURCES.sha256)
old "$T/leds/zrn-flash.c"

last_flash() { sh "$PICK" "$T" 2> /dev/null | grep zrn-flash.ko | tail -n 1; }

[ "$(last_flash)" = "$T/prebuilts/modules/zrn-flash.ko" ] && ok "fresh clone: the prebuilt one" || bad "fresh clone"

ko "$T/leds/zrn-flash.ko"; new "$T/leds/zrn-flash.ko"
[ "$(last_flash)" = "$T/leds/zrn-flash.ko" ] && ok "a module built after its source wins" || bad "fresh local build"

old "$T/leds/zrn-flash.ko"; new "$T/leds/zrn-flash.c"
out=$(sh "$PICK" "$T" 2>&1)
[ "$(last_flash)" = "$T/prebuilts/modules/zrn-flash.ko" ] && grep -q 'WARNING: not using leds/zrn-flash.ko' <<< "$out" &&
	ok "a stale local module is skipped, with a warning" || bad "stale local" "$out"

echo 'a changed source' > "$T/leds/zrn-flash.c"; new "$T/leds/zrn-flash.c"
out=$(sh "$PICK" "$T" 2>&1)
[ $? != 0 ] && grep -q 'out of date' <<< "$out" && ok "changed source, stale local, old prebuilt: the build stops" || bad "stale prebuilt" "$out"

touch -d '2026-07-01 00:00' "$T/leds/zrn-flash.ko"
[ "$(last_flash)" = "$T/leds/zrn-flash.ko" ] && ok "changed source with a new local build: goes on" || bad "new local build"

mkdir "$T/set"; ko "$T/set/zrn-flash.ko"; rm "$T/leds/zrn-flash.ko"
[ "$(ZTOS_MODULES=$T/set sh "$PICK" "$T" 2> /dev/null | grep zrn-flash.ko | tail -n 1)" = "$T/set/zrn-flash.ko" ] &&
	ok "ZTOS_MODULES has the last word" || bad "ZTOS_MODULES"
rm -r "$T/set"

echo 'the source' > "$T/leds/zrn-flash.c"
ko "$T/prebuilts/modules/zrn-led.ko" 6.6.0-other
out=$(sh "$PICK" "$T" 2>&1)
[ $? != 0 ] && grep -q 'was not built for kernel' <<< "$out" && ok "a module for another kernel stops the build" || bad "vermagic" "$out"

# the real thing: every prebuilt is for this kernel, and zrn-flash matches its source
out=$(sh "$PICK" "$ROOT" 2>&1)
if [ $? = 0 ] && grep -q 'prebuilts/modules/zrn-flash.ko' <<< "$out"; then
	ok "this repository's prebuilts pass, zrn-flash among them"
else
	bad "this repository's prebuilts" "$out"
fi

[ "$fails" = 0 ] && echo "all passed" || { echo "$fails failed"; exit 1; }
