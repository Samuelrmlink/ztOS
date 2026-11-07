#!/bin/bash
# ztos-variant against fake roots: the order of sources, and that a bad value
# in one falls through to the next. Needs no phone.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
V=$HERE/overlay/usr/local/sbin/ztos-variant
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0

check() {	# name, wanted output, wanted status, efs file, vendor model, system model
	local r=$T/$1 got st
	mkdir -p "$r/run/android/mnt/vendor/efs/imei" "$r/run/android/vendor" "$r/mnt/system/system"
	[ -n "$4" ] && printf '%s' "$4" > "$r/run/android/mnt/vendor/efs/imei/prodcode.dat"
	[ -n "$5" ] && printf 'x=1\nro.product.vendor.model=%s\n' "$5" > "$r/run/android/vendor/build.prop"
	[ -n "$6" ] && printf 'ro.product.system.model=%s\n' "$6" > "$r/mnt/system/system/build.prop"
	got=$(ZTOS_VARIANT_ROOT=$r sh "$V"); st=$?
	if [ "$got" = "$2" ] && [ "$st" = "$3" ]; then
		echo "ok   $1"
	else
		echo "FAIL $1: got '$got' status $st, wanted '$2' status $3"; fail=1
	fi
}

check f-efs           SM-G960F 0 SM-G960FABCDEF '' ''
check n-efs           SM-G960N 0 SM-G960NABCDEF '' ''
check efs-wins        SM-G960F 0 SM-G960FABCDEF SM-G960N SM-G960N
check n-vendor        SM-G960N 0 '' SM-G960N ''
check f-vendor        SM-G960F 0 '' SM-G960F SM-G960N
check n-system        SM-G960N 0 '' '' SM-G960N
check bad-efs         SM-G960N 0 'garbage-file' SM-G960N ''
check other-phone     unknown  1 SM-G965FABCDEF SM-G965F SM-G965F
check nothing         unknown  1 '' '' ''

# the wait gives up, and then uses VENDOR
if [ "$(ZTOS_VARIANT_ROOT=$T/n-vendor sh "$V" --wait 1)" = SM-G960N ]; then
	echo "ok   wait-falls-back"
else
	echo "FAIL wait-falls-back"; fail=1
fi
# the factory code past the model must never come out
out=$(ZTOS_VARIANT_ROOT=$T/f-efs sh "$V")
case "$out" in
*ABCDEF*) echo "FAIL leaks the product code"; fail=1 ;;
*) echo "ok   prints the model only" ;;
esac
exit $fail
