#!/usr/bin/env bash
# bt-images.sh prepare|swap|rollback: Bluetooth into the Waydroid images that
# are already installed, without rebuilding them from the stock ones (a new
# set-up gets it from strip-image.sh and patch-vendor.sh).
#
#   prepare   system.img.bt and vendor.img.bt: patched copies of the live
#             images. Android may keep running; the live images aren't touched.
#   swap      the copies become the images; the old ones are kept as
#             *.pre-bt. Android must be stopped (the container STOPPED).
#   rollback  the *.pre-bt images back. Android must be stopped.
#
# PATCHER is patch-bluetooth.py and PREBUILTS its prebuilts (defaults: the
# package's, else next to this checkout).
set -euo pipefail
[[ $EUID == 0 ]] || { echo 'Run as root.' >&2; exit 1; }
here=$(cd -- "$(dirname -- "$0")" && pwd)
images=${IMAGES:-/usr/share/waydroid-extra/images}
patcher=${PATCHER:-$here/../patch-bluetooth.py}
[[ -f $patcher ]] || patcher=/usr/share/ztos-waydroid/patch-bluetooth.py
prebuilts=${PREBUILTS:-}

stopped() {
	[[ $(lxc-info -P /var/lib/waydroid/lxc -n waydroid -sH 2>/dev/null || echo STOPPED) == STOPPED ]] ||
		{ echo 'Stop the Waydroid session and container first.' >&2; exit 1; }
}

patch_copy() {   # patch_copy NAME MODE
	local name=$1 mode=$2 work rc=0
	work=$(mktemp -d "$images/.bt.XXXXXX")
	trap 'mountpoint -q "$work/root" && umount "$work/root"; rm -rf -- "$work"' RETURN
	cp --reflink=auto --sparse=always --preserve=all "$images/$name.img" "$work/$name.img"
	# stock images can have no free block at all; the kernel keeps about 2%
	# (at most 16 MiB) of an ext4 to itself, so a few small files need 16 MiB
	rc=0
	e2fsck -fy "$work/$name.img" || rc=$?
	((rc <= 1))
	local blocks bsize
	blocks=$(tune2fs -l "$work/$name.img" | awk '/^Block count:/ {print $3}')
	bsize=$(tune2fs -l "$work/$name.img" | awk '/^Block size:/ {print $3}')
	resize2fs "$work/$name.img" "$((blocks + 16 * 1024 * 1024 / bsize))"
	mkdir "$work/root"
	mount -o loop,rw "$work/$name.img" "$work/root"
	rc=0
	python3 "$patcher" "$mode" "$work/root" $prebuilts || rc=$?
	umount "$work/root"
	((rc == 0)) || { echo "$name: patch-bluetooth.py failed ($rc)" >&2; return "$rc"; }
	rc=0
	e2fsck -fy "$work/$name.img" || rc=$?
	((rc <= 1))
	mv -T "$work/$name.img" "$images/$name.img.bt"
	echo "$images/$name.img.bt ready"
}

case ${1:-} in
prepare)
	patch_copy system system
	patch_copy vendor vendor
	;;
swap)
	stopped
	for n in system vendor; do [[ -f $images/$n.img.bt ]] || { echo "no $n.img.bt: prepare first" >&2; exit 1; }; done
	for n in system vendor; do
		mv -T "$images/$n.img" "$images/$n.img.pre-bt"
		mv -T "$images/$n.img.bt" "$images/$n.img"
	done
	echo 'swapped: start Android again'
	;;
rollback)
	stopped
	for n in system vendor; do
		[[ -f $images/$n.img.pre-bt ]] && mv -T "$images/$n.img.pre-bt" "$images/$n.img"
	done
	echo 'rolled back'
	;;
*)
	sed -n '2,13p' "$0" >&2
	exit 2
	;;
esac
