#!/bin/bash
# Build zrn-cam.ko against the phone's running kernel.
#
#   camera/driver/build.sh [kernel tree] [kernel output dir]
#
# The tree is the one the phone's kernel was built from, with the port applied
# (scripts/build-bt-modules.sh builds the same set; this builds only zrn-cam).
# Defaults: build/linux-7.1-dp, out-target (the target kernel's own config). This kernel has no MODVERSIONS,
# so the build stops unless that output's config has the same module ABI
# (scripts/module-abi.sh) as the target: set ZRN_CAM_CONFIG to the target's
# config (zcat /proc/config.gz from the phone) to check against it.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
MAIN=$(cd "$ROOT" && git rev-parse --path-format=absolute --git-common-dir 2>/dev/null | sed 's,/\.git$,,')
MAIN=${MAIN:-$ROOT}
SRC=$(realpath "${1:-${KSRC:-$MAIN/build/linux-7.1-dp}}")
O=${2:-out-target}
TC=$MAIN/build/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin
export PATH=$TC:$PATH LC_ALL=C ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu-
REL=$(cat "$SRC/$O/include/config/kernel.release")
ABI=$(sh "$ROOT/scripts/module-abi.sh" "$SRC/$O/.config")
if [ -n "${ZRN_CAM_CONFIG:-}" ]; then
	want=$(sh "$ROOT/scripts/module-abi.sh" "$ZRN_CAM_CONFIG")
	[ "$want" = "$ABI" ] || { echo "ABI $ABI of $SRC/$O is not the target's $want" >&2; exit 1; }
fi
find "$HERE" \( -name '*.o' -o -name '*.ko' -o -name '*.mod' -o -name '*.mod.c' \
	-o -name '.*.cmd' -o -name modules.order -o -name Module.symvers \) -delete
make -s -C "$SRC" O="$O" M="$HERE" KBUILD_MODPOST_WARN=1 modules
v=$(modinfo -F vermagic "$HERE/zrn-cam.ko")
case "$v" in "$REL "*) ;; *) echo "vermagic '$v', not $REL" >&2; exit 1 ;; esac
echo "BUILT $HERE/zrn-cam.ko for $REL, ABI $ABI"
