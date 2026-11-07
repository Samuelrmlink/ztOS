#!/bin/sh
# Which kernel modules go on the phone: prints their paths, later ones replacing
# earlier ones of the same name. Run by layer.sh, with the repository as $1.
#
#   1. prebuilts/modules/*.ko, what a fresh clone has.
#   2. A module built in this tree, but only if it is newer than the source beside
#      it. *.ko files are not in git, so one left from an earlier build survives a
#      pull that changes its source, and would go in silently: it is skipped, with
#      a warning. (It goes by file times, so it can't see a module built from another
#      branch's source when that source has not been touched since.)
#   3. $ZTOS_MODULES, a directory from scripts/build-bt-modules.sh, if set: modules
#      built for the very kernel being installed.
#
# It stops the build if a module was built for another kernel release, or if a
# prebuilt module whose source is listed in prebuilts/modules/SOURCES.sha256 no
# longer matches that source and nothing newer replaces it. That list is how a fix
# living in a module (zrn-flash and the sensor hub, ROADMAP 1.2) can't be lost by
# accident. To change such a source: rebuild the module, copy it into
# prebuilts/modules/, and run: sha256sum leds/zrn-flash.c > prebuilts/modules/SOURCES.sha256
set -eu
ROOT=${1:?the repository}
KERNEL=7.1.0-zboot
LOCAL="modem/zrn-modem/zrn-modem.ko power/idle/zrn-cpuidle.ko haptics/zrn-haptic.ko
	power/hotplug/zrn-hotplug.ko leds/zrn-led.ko leds/zrn-flash.ko sensors/driver/zrn-bbd.ko
	audio/driver/snd-zrn-abox.ko audio/modules/*.ko score/driver/zrn-score.ko"

current() {	# module: built since anything beside it changed?
	[ -z "$(find "$(dirname "$1")" -maxdepth 1 -type f \
		\( -name '*.c' -o -name '*.h' -o -name Makefile -o -name Kbuild \) \
		-newer "$1" 2> /dev/null | head -n 1)" ]
}

check_kernel() {	# module
	grep -qa "vermagic=$KERNEL " "$1" || {
		echo "pick-modules: $1 was not built for kernel $KERNEL" >&2
		exit 1
	}
}

picked=
take() {
	check_kernel "$1"
	picked="$picked $1"
	echo "$1"
}

for k in "$ROOT"/prebuilts/modules/*.ko; do
	[ -f "$k" ] && take "$k"
done

cd "$ROOT"
for k in $LOCAL; do
	[ -f "$k" ] || continue
	if current "$k"; then
		take "$ROOT/$k"
	else
		echo "WARNING: not using $k: its source is newer than it (rebuild it, or the prebuilt one is used)" >&2
	fi
done

if [ -n "${ZTOS_MODULES:-}" ]; then
	[ -d "$ZTOS_MODULES" ] || { echo "pick-modules: ZTOS_MODULES=$ZTOS_MODULES is not a directory" >&2; exit 1; }
	for k in "$ZTOS_MODULES"/*.ko; do
		[ -f "$k" ] && take "$k"
	done
fi

# Is each module with a recorded source still that source's, or replaced by a newer build?
if [ -f prebuilts/modules/SOURCES.sha256 ]; then
	while read -r sum src; do
		[ -n "$sum" ] || continue
		[ "$(sha256sum "$src" | cut -d' ' -f1)" != "$sum" ] || continue
		name=$(basename "${src%.c}").ko
		last=
		for k in $picked; do
			[ "$(basename "$k")" = "$name" ] && last=$k
		done
		case $last in
		"$ROOT"/prebuilts/*|'')
			echo "pick-modules: prebuilts/modules/$name is out of date: $src has changed since it was built." >&2
			echo "Rebuild it (scripts/build-bt-modules.sh), and see the top of devices/samsung-starlte/pick-modules.sh." >&2
			exit 1
			;;
		esac
	done < prebuilts/modules/SOURCES.sha256
fi
