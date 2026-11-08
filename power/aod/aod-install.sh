#!/bin/bash
# aod-install.sh: put feature/aod's shell, Settings and sensor daemon in place on
# the phone and start the session again in them. Run ON the phone, as root, once
# the tree is built there:
#
#   cd ~/ztdev-aod/ztmetro && make -j2 ztmetro settings/ztsettings sensord/ztoscsf-sensord
#   sudo bash ~/ztdev-aod/power/aod/aod-install.sh [BUILD_DIR [--no-restart]]
#   sudo bash ~/ztdev-aod/power/aod/aod-install.sh --undo
#
# The binaries it replaces are kept as *.pre-aod (the first time only), and
# --undo puts them back. No kernel, panel or boot image change: the always-on
# display stays off until Settings > Display > Always-on display is switched
# on (or the shell's ctl is told "aod on"). The sensor daemon takes "aod MS"
# (the light sensor kept on, slowly, with the screen off); an old one ignores it.
set -euo pipefail
BIN=/usr/local/bin
SBIN=/usr/local/sbin
if [ "${1:-}" = --undo ]; then
	for f in $BIN/ztmetro $BIN/ztsettings $SBIN/ztoscsf-sensord; do
		[ -e "$f.pre-aod" ] && install -m755 "$f.pre-aod" "$f" && echo "restored $f"
	done
	systemctl restart ztoscsf-sensord || true
else
	BUILD=${1:-$(dirname "$(readlink -f "$0")")/../../ztmetro}
	for f in ztmetro settings/ztsettings sensord/ztoscsf-sensord; do
		[ -x "$BUILD/$f" ] || { echo "no $BUILD/$f: build it first" >&2; exit 1; }
		file "$BUILD/$f" | grep -q aarch64 || { echo "$BUILD/$f is not an aarch64 program" >&2; exit 1; }
	done
	for f in $BIN/ztmetro $BIN/ztsettings $SBIN/ztoscsf-sensord; do
		[ -e "$f.pre-aod" ] || cp -p "$f" "$f.pre-aod"
	done
	install -m755 "$BUILD/ztmetro" "$BIN/ztmetro"
	install -m755 "$BUILD/settings/ztsettings" "$BIN/ztsettings"
	install -m755 "$BUILD/sensord/ztoscsf-sensord" "$SBIN/ztoscsf-sensord"
	systemctl restart ztoscsf-sensord
	echo "installed from $BUILD"
fi
# Start the shell again: only greetd's own session (its scope: the shell and the apps it
# started) ends, never the user's other processes (ssh logins, builds, user services), as
# zboot-session's "loginctl terminate-user; pkill -9 -u" would. With --no-restart, nothing.
[ "${2:-}" = --no-restart ] || [ "${1:-}" = --no-restart ] && exit 0
s=$(loginctl list-sessions --no-legend | while read -r id _ _ seat _; do
	[ "$(loginctl show-session "$id" -p Service --value)" = greetd ] && echo "$id"
done | head -1)
systemctl stop greetd
if [ -n "$s" ]; then
	loginctl terminate-session "$s"
	for i in $(seq 20); do loginctl show-session "$s" > /dev/null 2>&1 || break; sleep 0.25; done
	systemctl kill -s KILL "session-$s.scope" 2> /dev/null || true
fi
rm -f /run/greetd.run	# so the start logs in again ([initial_session])
systemctl start greetd
sleep 3
n=$(loginctl list-sessions --no-legend | while read -r id _; do
	[ "$(loginctl show-session "$id" -p Service --value)" = greetd ] && echo "$id"
done | head -1)
echo "shell session: was ${s:-none}, now ${n:-none} ($(loginctl show-session "${n:-0}" -p Service -p Remote -p Type 2>/dev/null | tr '\n' ' '))"
