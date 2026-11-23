#!/bin/bash
# On the phone, as the session user, inside `ztlock run`: grim screenshots
# after a shell install: the lock screen (with the sleep-blocker line), its
# sheet, the action centre with the Sleep and Location tiles (the quick list
# set for the shot and put back), Settings' lock screen, display and battery
# pages, and the always-on display. Into OUT (default ~/shots-integrated).
set -u
OUT=${1:-$HOME/shots-integrated}
mkdir -p "$OUT"
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}
CTL=$XDG_RUNTIME_DIR/ztmetro.ctl
CONF=$HOME/.config/ztmetro/config
SHELL_PID=$(pgrep -x -u "$(id -u)" ztmetro | head -1)
c() { echo "$*" > "$CTL"; }
# (grim waits for a frame, and an idle shell draws none: a status redraw)
shot() { grim "$OUT/$1.png" & g=$!; sleep 0.3; c modem; wait $g && echo "shot $OUT/$1.png"; }
cp -a "$CONF" "$CONF.integrated-shots"
restore() {
	cp -a "$CONF.integrated-shots" "$CONF" && rm -f "$CONF.integrated-shots"
	kill -HUP "$SHELL_PID"
	[ -n "${SETTINGS:-}" ] && kill "$SETTINGS" 2>/dev/null
}
trap restore EXIT

c screen off; sleep 2
shot aod
c screen on; sleep 4		# locked, and zrn-sleep-why asked
shot lock
# the awake line sits just over the clock block: tap it (raised with notifications)
c tap 80 443; sleep 3; shot lock-sheet		# at rest
c tap 180 150; sleep 1				# closed (or a lift if it was not open)
c tap 80 422; sleep 3; shot lock-sheet-raised	# with notifications waiting
c tap 180 150; sleep 1
# unlock: a swipe up
c down 1 180 650; for y in 600 550 500 450 400 350 300 250 200 150; do sleep 0.03; c move 1 180 $y; done; c up 1; sleep 2
# the tiles for the shot: Sleep and Location among them (put back at the end)
sed -i '/^quick *=/d' "$CONF"
echo "quick = wifi,data,bluetooth,sleep,location,audio,brightness,settings" >> "$CONF"
kill -HUP "$SHELL_PID"; sleep 2
c down 2 180 3; for y in 60 160 260 360 460; do sleep 0.03; c move 2 180 $y; done; c up 2; sleep 3
shot quick-tiles
c tap 180 186; sleep 2; shot quick-editor	# EDIT: the editor (8.49)
c tap 310 190; sleep 1				# DONE
# turned (a test turn, as auto-rotation would): Start, and the panel
c key 158 down; c key 158 up; sleep 1
c rotate 3; sleep 2; shot start-turned
c down 0 370 2; for x in 60 120 200 300; do sleep 0.03; c move 0 370 $x; done; c up 0; sleep 2; shot shade-turned
c tap 370 128; sleep 2; shot quick-editor-turned
c tap 680 132; sleep 1
c key 158 down; c key 158 up; sleep 1
c rotate auto; sleep 2
c key 158 down; c key 158 up; sleep 1	# back: the panel closes
for page in lock display battery; do
	if [ -z "${SETTINGS:-}" ]; then ztsettings $page & SETTINGS=$!; sleep 5; else ztsettings $page; sleep 3; fi
	shot settings-$page
done
c screen off
