#!/bin/sh
# Put zrn-cam on the phone, as root, from a directory holding zrn-cam.ko,
# 90-zrn-cam.rules and zrn-cam-probe (camera/driver after build.sh):
#
#   sudo sh install.sh            install; the camera app loads it on first open
#   sudo sh install.sh --hold     install, but nothing loads it by itself yet
#   sudo sh install.sh --release  lift the hold
#
# Nothing loads zrn-cam at boot. The module's alias devname:zrn-cam makes
# kmod-static-nodes create /dev/zrn-cam (c 10:250) at boot; the first open of it
# (the Camera app) loads the module through request_module. Loading alone only
# maps registers and registers the device; the sensor is powered for a stream
# and powered off when the stream ends or the last reader closes.
#
# The hold is /etc/modprobe.d/zrn-cam-hold.conf ("blacklist zrn-cam"): an open
# then gets ENODEV and the app shows its test scene. modprobe zrn-cam by hand
# still works. Backups of anything replaced: NAME.pre-camera.
set -eu
D=$(cd "$(dirname "$0")" && pwd)
REL=$(uname -r)
M=/usr/lib/modules/$REL/extra
HOLD=/etc/modprobe.d/zrn-cam-hold.conf
[ "$(id -u)" = 0 ] || { echo "as root" >&2; exit 1; }
if [ "${1:-}" = --release ]; then
	rm -f "$HOLD"
	if [ ! -e /dev/zrn-cam ]; then
		mknod -m 0660 /dev/zrn-cam c 10 250
		chgrp video /dev/zrn-cam
	fi
	echo "hold lifted: opening /dev/zrn-cam loads zrn-cam"
	exit 0
fi
v=$(modinfo -F vermagic "$D/zrn-cam.ko")
case "$v" in "$REL "*) ;; *) echo "zrn-cam.ko is for '$v', not $REL" >&2; exit 1 ;; esac
if [ "${1:-}" = --hold ]; then
	printf '%s\n' '# zrn-cam waits for approval: no load by alias (opening /dev/zrn-cam).' \
		'# Remove this file (camera/driver/install.sh --release) to allow it.' 'blacklist zrn-cam' > "$HOLD"
fi
backup() { [ -e "$1" ] && [ ! -e "$1.pre-camera" ] && cp -a "$1" "$1.pre-camera"; return 0; }
backup "$M/zrn-cam.ko"
backup /etc/udev/rules.d/90-zrn-cam.rules
backup /usr/local/sbin/zrn-cam-probe
install -Dm644 "$D/zrn-cam.ko" "$M/zrn-cam.ko"
install -Dm644 "$D/90-zrn-cam.rules" /etc/udev/rules.d/90-zrn-cam.rules
install -Dm755 "$D/zrn-cam-probe" /usr/local/sbin/zrn-cam-probe
depmod -a "$REL"
udevadm control --reload 2>/dev/null || true
# the node for this boot; later boots get it from kmod-static-nodes and udev
if [ ! -e /dev/zrn-cam ] && [ ! -e "$HOLD" ]; then
	mknod -m 0660 /dev/zrn-cam c 10 250
	chgrp video /dev/zrn-cam
fi
echo "installed $M/zrn-cam.ko ($(sha256sum "$M/zrn-cam.ko" | cut -c1-16)); alias: $(grep zrn_cam "/usr/lib/modules/$REL/modules.devname")"
[ -e "$HOLD" ] && echo "held: $HOLD" || true
