#!/bin/bash
# Build snd-zrn-abox.ko for the phone's running kernel (7.1.0-zboot-wd-dp6).
#
#   audio/driver/build-target.sh [kernel tree] [output dir]
#
# Sound is modular on that kernel: its snd*.ko come from out-target-snd (the
# target config plus CONFIG_SND=m), so this module is built against that
# output, as the one installed now was. Checks the vermagic against it.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
MAIN=$(cd "$ROOT" && git rev-parse --path-format=absolute --git-common-dir 2>/dev/null | sed 's,/\.git$,,')
MAIN=${MAIN:-$ROOT}
SRC=$(realpath "${1:-$MAIN/build/linux-7.1-dp}")
O=${2:-out-target-snd}
TC=$MAIN/build/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin
export PATH=$TC:$PATH LC_ALL=C ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu-
REL=$(cat "$SRC/$O/include/config/kernel.release")
find "$HERE" \( -name '*.o' -o -name '*.ko' -o -name '*.mod' -o -name '*.mod.c' \
	-o -name '.*.cmd' -o -name modules.order -o -name Module.symvers \) -delete
make -s -C "$SRC" O="$O" M="$HERE" KBUILD_MODPOST_WARN=1 modules 2>&1 |
	grep -vE 'pahole version|The kernel was built with|You are using' || true
v=$(modinfo -F vermagic "$HERE/snd-zrn-abox.ko")
case "$v" in "$REL "*) ;; *) echo "vermagic '$v', not $REL" >&2; exit 1 ;; esac
echo "BUILT $HERE/snd-zrn-abox.ko for $REL"
