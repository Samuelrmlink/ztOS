#!/bin/sh
# The desktop's tests (desktop/dev/test-*.sh) on the phone itself, against the
# running shell: on a virtual desktop (added for the run if there is none),
# with the phone's own apps for the windows (no foot there).
#
#   cd ~/ztmetro && sh desktop/dev/phone-check.sh windows|env|mover|latency|all
#
# Run it from a build of this tree (the tools: desktop/zt-deskpoke,
# zt-desklat, ztmove, pods/ztpods), inside `ztlock run`. The phone must be
# unlocked: a locked desktop takes no pointer (that is the point of its lock).
# Screenshots go to ~/.cache/ztdesk-check/.
set -u
cd "$(dirname "$0")/../.."
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}
ctl() { echo "$*" > "$XDG_RUNTIME_DIR/ztmetro.ctl"; sleep 0.3; }
state=$XDG_RUNTIME_DIR/ztmetro-desktop.state
ctl desktop state
if grep -q '^locked yes' "$state"; then
	echo "the desktop is locked (it locked with the phone): unlock the phone, then run this again"
	exit 2
fi
added=
if grep -q '^output none' "$state"; then
	ctl desktop add 1920x1080
	added=1
	sleep 1
fi
started=
if grep -q '^session stopped' "$state"; then
	ctl desktop start
	started=1
	sleep 10
fi
ctl desktop watch 900
export OUTDIR=$HOME/.cache/ztdesk-check LOG=$HOME/.local/state/ztmetro.log PANEL=${PANEL:-DSI-1}
mkdir -p "$OUTDIR"
export SSD_CMD="ztmove" SSD_ID=ztmove PHONE_CMD="xfce4-terminal --disable-server --title=phonecheck" \
	PHONE_ID=xfce4-terminal
rc=0
for t in ${1:-all}; do
	[ "$t" = all ] && t="windows env mover latency"
	for one in $t; do
		echo "== $one"
		sh "desktop/dev/test-$one.sh" | tee "$OUTDIR/$one.log"
		grep -q '^FAIL' "$OUTDIR/$one.log" && rc=1
	done
done
ctl desktop watch 0
[ -n "$started" ] && ctl desktop stop
[ -n "$added" ] && ctl desktop remove
exit $rc
