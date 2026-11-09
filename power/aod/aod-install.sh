#!/bin/bash
# aod-install.sh: put feature/aod's shell, Settings, sensor daemon and the apps
# it touches in place on the phone, and start the session again in them. Run ON
# the phone, as root, once the tree is built there:
#
#   cd ~/ztdev-aod/ztmetro && nice -n 19 make -j2 ztmetro settings/ztsettings \
#       sensord/ztoscsf-sensord store/ztstore gallery/ztgallery ntfy/ztntfy \
#       osial/ztosial files/ztfiles pods/ztpods sound/ztsound
#   sudo bash ~/ztdev-aod/power/aod/aod-install.sh [BUILD_DIR] [--no-restart] [--refresh-backup]
#   sudo bash ~/ztdev-aod/power/aod/aod-install.sh --undo [--no-restart]
#
# The binaries it replaces are kept as *.pre-aod: the first time only, or, with
# --refresh-backup, the ones installed now (so --undo goes back one install).
# --undo puts them back. --no-restart restarts nothing (not the shell, not the
# sensor daemon): a reboot picks it all up. No kernel, panel or boot image change.
set -euo pipefail
BIN=/usr/local/bin
SBIN=/usr/local/sbin
# built file (under BUILD) : where it goes
FILES="ztmetro:$BIN/ztmetro settings/ztsettings:$BIN/ztsettings sensord/ztoscsf-sensord:$SBIN/ztoscsf-sensord
store/ztstore:$BIN/ztstore gallery/ztgallery:$BIN/ztgallery ntfy/ztntfy:$BIN/ztntfy osial/ztosial:$BIN/ztosial
files/ztfiles:$BIN/ztfiles pods/ztpods:$BIN/ztpods sound/ztsound:$BIN/ztsound"
UNDO=0 RESTART=1 REFRESH=0 BUILD=
for a in "$@"; do
	case "$a" in
	--undo) UNDO=1 ;;
	--no-restart) RESTART=0 ;;
	--refresh-backup) REFRESH=1 ;;
	*) BUILD=$a ;;
	esac
done
if [ $UNDO = 1 ]; then
	for pair in $FILES; do
		f=${pair#*:}
		[ -e "$f.pre-aod" ] && install -m755 "$f.pre-aod" "$f" && echo "restored $f"
	done
	[ $RESTART = 1 ] && { systemctl restart ztoscsf-sensord || true; }
else
	BUILD=${BUILD:-$(dirname "$(readlink -f "$0")")/../../ztmetro}
	for pair in $FILES; do
		src=$BUILD/${pair%%:*}
		[ -x "$src" ] || { echo "no $src: build it first" >&2; exit 1; }
		file "$src" | grep -q aarch64 || { echo "$src is not an aarch64 program" >&2; exit 1; }
	done
	for pair in $FILES; do
		f=${pair#*:}
		if [ -e "$f" ] && { [ $REFRESH = 1 ] || [ ! -e "$f.pre-aod" ]; }; then
			cp -p "$f" "$f.pre-aod"
		fi
	done
	for pair in $FILES; do
		install -m755 "$BUILD/${pair%%:*}" "${pair#*:}"
	done
	[ $RESTART = 1 ] && systemctl restart ztoscsf-sensord
	echo "installed from $BUILD"
fi
# Start the shell again: only greetd's own session (its scope: the shell and the apps it
# started) ends, never the user's other processes (ssh logins, builds, user services), as
# zboot-session's "loginctl terminate-user; pkill -9 -u" would. With --no-restart, nothing.
[ $RESTART = 1 ] || exit 0
set +e +o pipefail	# from here every step is tried, whatever the one before did
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
