#!/bin/sh
# Auto-rotation (ROADMAP 8.33) on a PC: the phone shell alone on wlroots'
# headless backend, at the S9's shape (720x1480, scale 2), swiped unlocked, turned
# with the control pipe's `rotate N` and captured in each orientation.
# Captures, the state file and the log go to out/rotation/.
#
#   ztmetro/desktop/dev/rotation.sh            the standard set of shots
#   ztmetro/desktop/dev/rotation.sh 'CMDS'     these instead (c LINE, shot NAME)
#
# grim captures the panel as it is scanned out (portrait); each shot is also
# saved turned upright as NAME-seen.png, as a person holding the phone sees it.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
img=localhost/ztos-desktop-dev
podman image exists $img || podman build -t $img "$here"
out="$root/out/rotation"
mkdir -p "$out"
cmd=${1:-'
c rotate 0; sleep 1; shot start-portrait
c rotate 1; sleep 1.5; shot start-left
c rotate 3; sleep 1.5; shot start-right
c down 0 370 2; c move 0 370 120; c move 0 370 300; c up 0; sleep 1; shot shade-right
c down 0 370 330; c move 0 370 200; c move 0 370 20; c up 0; sleep 1
c down 0 600 200; c move 0 450 200; c move 0 200 200; c up 0; sleep 1; shot list-right
c down 0 100 200; c move 0 300 200; c move 0 600 200; c up 0; sleep 1
c rotate 0; sleep 1; c launch foot; sleep 3; shot app-portrait
c rotate 1; sleep 2; shot app-left
c rotate 1; c screen off; sleep 1; c screen on; sleep 1.5; shot locked-after-wake
c rotate auto; sleep 1; shot auto-off
'}
cat > "$out/.session.sh" <<EOF
c() { echo "\$*" > \$XDG_RUNTIME_DIR/ztmetro.ctl; sleep 0.2; }
shot() {
	# (grim waits for a frame, and an idle shell draws none: a status redraw)
	grim -o HEADLESS-1 /out/\$1.png & g=\$!; sleep 0.3; c modem; wait \$g
	grep -E "^(points|rotation|orientation)" \$XDG_RUNTIME_DIR/ztmetro.state | tr '\n' ' ' > /out/\$1.state
}
mkdir -p \$HOME/.config/ztmetro
printf 'timeout = 0\n' > \$HOME/.config/ztmetro/config
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 ZTMETRO_SIZE=720x1480 ./ztmetro > /out/ztmetro.log 2>&1 &
sleep 5
export WAYLAND_DISPLAY=wayland-0
c screen on
sleep 1
# swipe the lock screen away (no PIN here)
c down 0 180 700; c move 0 180 500; c move 0 180 250; c move 0 180 60; c up 0
sleep 1.5
$cmd
EOF
podman run --rm --privileged --device /dev/dri/renderD128 --group-add keep-groups \
	-v "$root:/src:Z" -v "$out:/out:Z" -w /src/ztmetro $img sh -c '
	make ztmetro >/dev/null
	export XDG_RUNTIME_DIR=/tmp/rt HOME=/tmp/home
	mkdir -p $XDG_RUNTIME_DIR $HOME && chmod 700 $XDG_RUNTIME_DIR
	dbus-uuidgen > /etc/machine-id
	dbus-run-session -- sh /out/.session.sh 2>&1 | tee /out/session.out'
# upright, as seen: rotation N is the phone turned N quarters counter-clockwise,
# so the capture is turned the same way to stand it up
for s in "$out"/*.state; do
	n=$(basename "$s" .state)
	r=$(sed -n 's/.*rotation \([0-3]\).*/\1/p' "$s")
	python3 -I "$here/png-turn.py" "$out/$n.png" "$out/$n-seen.png" "${r:-0}" 2>/dev/null || true
	echo "$n: $(cat "$s")"
done
