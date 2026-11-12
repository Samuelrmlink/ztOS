#!/bin/sh
# Build zrn-nfc.ko against the running kernel's tree (7.1.0-zboot-wd-dp6).
# KOUT=<out dir> overrides the tree.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${ZTOS_ROOT:-/home/samuel/git-clone/ztOS}
KOUT=${KOUT:-$ROOT/build/linux-7.1-dp/out}
TC=$ROOT/build/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin
export PATH="$TC:$PATH"
make -C "$KOUT" M="$HERE" ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- modules
