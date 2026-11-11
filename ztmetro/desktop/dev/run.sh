#!/bin/sh
# Desktop mode on a PC, without a phone: ztmetro built in a container and run
# on wlroots' headless backend. HEADLESS-1 is the phone's panel, HEADLESS-2
# the desktop (1600x900), with the XFCE session on it. Captures and logs go
# to out/desktop/.
#
#   ztmetro/desktop/dev/run.sh                 start, capture both screens, stop
#   ztmetro/desktop/dev/run.sh 'CMD; CMD'      and run these first (in the session)
#   ztmetro/desktop/dev/run.sh sh              stay in a shell in the session
#
# In the commands, `c LINE` writes to the shell's control pipe (desktop move
# foot, desktop click 800 450, ...), `shot NAME` captures both screens, `poke
# 'drag X0 Y0 X1 Y1'` drives ztdesk's kind of pointer (desktop/zt-deskpoke.c)
# and `win APP` prints the APP's window line from the state file.
# Needs podman and a render node: the desktop is composited on the GPU.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
img=localhost/ztos-desktop-dev
podman image exists $img || podman build -t $img "$here"
mkdir -p "$root/out/desktop"
cat > "$root/out/desktop/.session.sh" <<EOF
c() { echo "\$*" > \$XDG_RUNTIME_DIR/ztmetro.ctl; }
shot() { grim -o HEADLESS-2 /out/\$1-desktop.png; grim -o HEADLESS-1 /out/\$1-phone.png; }
poke() { ./desktop/zt-deskpoke HEADLESS-2 1600 900 "\$*"; }
win() { c desktop state; sleep 0.2; grep "^window.*\$1" \$XDG_RUNTIME_DIR/ztmetro-desktop.state; }
# the phone starts locked, and nothing here can swipe it open: the desktop
# does not lock with it
export ZTMETRO_DESKTOP_NOLOCK=1
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 ZTMETRO_DESKTOP=1600x900 ./ztmetro > /out/ztmetro.log 2>&1 &
sleep 4
export WAYLAND_DISPLAY=wayland-0
c screen on; c demoloop on; c desktop watch 3600; c desktop start
sleep 8
${1:-true}
shot final
cat \$XDG_RUNTIME_DIR/ztmetro-desktop.state
EOF
tty=; [ -t 0 ] && tty=-it
exec podman run --rm $tty --privileged --device /dev/dri/renderD128 --group-add keep-groups \
	-v "$root:/src:Z" -v "$root/out/desktop:/out:Z" -w /src/ztmetro $img sh -c '
	make ztmetro desktop/zt-deskpoke >/dev/null
	export XDG_RUNTIME_DIR=/tmp/rt HOME=/tmp/home PATH=/src/ztmetro/desktop:$PATH
	mkdir -p $XDG_RUNTIME_DIR $HOME && chmod 700 $XDG_RUNTIME_DIR
	dbus-uuidgen > /etc/machine-id
	exec dbus-run-session -- sh /out/.session.sh'
