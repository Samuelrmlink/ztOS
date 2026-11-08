#!/bin/bash
# aod-install.sh: put feature/aod's shell and Settings in place on the phone
# and start the session again in it. Run ON the phone, as root, once the tree
# is built there:
#
#   cd ~/ztdev-aod/ztmetro && make -j8 ztmetro settings/ztsettings
#   sudo bash ~/ztdev-aod/power/aod/aod-install.sh [BUILD_DIR]
#   sudo bash ~/ztdev-aod/power/aod/aod-install.sh --undo
#
# The binaries it replaces are kept as *.pre-aod (the first time only), and
# --undo puts them back. No kernel, panel or boot image change: the always-on
# display stays off until Settings > Display > Always-on display is switched
# on (or the shell's ctl is told "aod on").
set -euo pipefail
BIN=/usr/local/bin
if [ "${1:-}" = --undo ]; then
	for f in ztmetro ztsettings; do
		[ -e "$BIN/$f.pre-aod" ] && install -m755 "$BIN/$f.pre-aod" "$BIN/$f" && echo "restored $f"
	done
else
	BUILD=${1:-$(dirname "$(readlink -f "$0")")/../../ztmetro}
	for f in ztmetro settings/ztsettings; do
		[ -x "$BUILD/$f" ] || { echo "no $BUILD/$f: build it first" >&2; exit 1; }
		file "$BUILD/$f" | grep -q aarch64 || { echo "$BUILD/$f is not an aarch64 program" >&2; exit 1; }
	done
	for f in ztmetro ztsettings; do
		[ -e "$BIN/$f.pre-aod" ] || cp -p "$BIN/$f" "$BIN/$f.pre-aod"
	done
	install -m755 "$BUILD/ztmetro" "$BIN/ztmetro"
	install -m755 "$BUILD/settings/ztsettings" "$BIN/ztsettings"
	echo "installed from $BUILD"
fi
/usr/local/sbin/zboot-session ztmetro | tail -1
