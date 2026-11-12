#!/bin/bash
# Build zrn-therm.ko on the workstation against the running kernel's tree
# (camera/driver/build.sh's pattern). KTREE: the configured kernel tree whose
# out/ matches `uname -r` on the phone (today build/linux-7.1-dp, 7.1.0-zboot-wd-dp6).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
BUILD=${ZTOS_BUILD:-$(cd "$ROOT" && git rev-parse --path-format=absolute --git-common-dir)/../build}
KTREE=${KTREE:-$BUILD/linux-7.1-dp}
export PATH="$BUILD/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH"
export ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- LC_ALL=C KBUILD_MODPOST_WARN=1
python3 -I "$HERE/gen-tables.py" > "$HERE/therm-tables.h.new"
cmp -s "$HERE/therm-tables.h.new" "$HERE/therm-tables.h" || { echo "build: therm-tables.h is out of date" >&2; exit 1; }
rm -f "$HERE/therm-tables.h.new"
make -C "$KTREE" O=out M="$HERE" modules
echo "release: $(cat "$KTREE/out/include/config/kernel.release")"
modinfo -F vermagic "$HERE/zrn-therm.ko"
