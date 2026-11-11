#!/bin/bash
# fetch.sh [SYSTEM_IMG]: get what build.sh needs into $ZTBT_WORK (default
# ~/ztbt-work): the pinned headers (sources.conf), the NDK (checked against its
# sha256), and, from the Waydroid system image, the libraries the shim links
# against (libc++.so, liblog.so) and the stock libbluetooth_jni.so. The image
# is read with debugfs, never mounted. Without SYSTEM_IMG, the libraries must
# already be in $ZTBT_WORK/image-libs.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/sources.conf"
work=${ZTBT_WORK:-$HOME/ztbt-work}
mkdir -p "$work/src" "$work/image-libs"

pinned() {   # pinned DIR REPO COMMIT [SPARSE PATHS...]
	local dir=$1 repo=$2 commit=$3
	shift 3
	if [[ ! -d $dir/.git ]]; then
		git init -q "$dir"
		git -C "$dir" remote add origin "$repo"
	fi
	if [[ $(git -C "$dir" rev-parse -q --verify HEAD 2>/dev/null) != "$commit" ]]; then
		if (($#)); then
			git -C "$dir" sparse-checkout set "$@"
		fi
		git -C "$dir" fetch -q --depth 1 --filter=blob:none origin "$commit"
		git -C "$dir" checkout -q --detach FETCH_HEAD
	fi
	[[ $(git -C "$dir" rev-parse HEAD) == "$commit" ]] || { echo "$dir: not at $commit" >&2; exit 1; }
}

pinned "$work/src/bt" "$LINEAGE_BT_REPO" "$LINEAGE_BT_COMMIT" system/include system/types system/gd/hci system/gd/os
pinned "$work/src/libchrome" "$LIBCHROME_REPO" "$LIBCHROME_COMMIT" base build testing

if [[ ! -x $work/$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin/clang ]]; then
	zip=$work/$(basename "$NDK_URL")
	[[ -f $zip ]] || curl -fL --retry 3 -o "$zip" "$NDK_URL"
	echo "$NDK_SHA256  $zip" | sha256sum -c -
	unzip -q -o "$zip" -d "$work"
fi

if (($#)); then
	img=$1
	for f in libc++.so liblog.so; do
		debugfs -R "dump /system/lib64/$f $work/image-libs/$f" "$img" 2>/dev/null
	done
	debugfs -R "dump /system/apex/com.android.btservices/lib64/libbluetooth_jni.so $work/image-libs/libbluetooth_jni.so" \
		"$img" 2>/dev/null
fi
for f in libc++.so liblog.so libbluetooth_jni.so; do
	[[ -s $work/image-libs/$f ]] || { echo "missing $work/image-libs/$f: give the system image" >&2; exit 1; }
done
echo "ready in $work"
