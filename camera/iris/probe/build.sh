#!/bin/bash
# Build zrn-iris-probe.ko against the phone's running kernel (7.1.0-zboot-wd-dp6),
# and the fingerprint probe (static aarch64), and run the host test.
# The kernel tree is build/linux-7.1-dp with O=out-target (the module build of it); its .config hashes the same as
# the phone's /proc/config.gz (scripts/module-abi.sh). KSRC/KOUT override it.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=${ZTOS_ROOT:-/home/samuel/git-clone/ztOS}
KSRC=${KSRC:-$ROOT/build/linux-7.1-dp}
KOUT=${KOUT:-out-target}
TC=$ROOT/build/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cc -Wall -Wextra -Werror -o "$TMP/test-iris-seq" "$HERE/test-iris-seq.c"
"$TMP/test-iris-seq"

export PATH="$TC:$PATH" ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- LC_ALL=C KBUILD_MODPOST_WARN=1
make -C "$KSRC" O="$KOUT" M="$HERE" W=1 modules
modinfo "$HERE/zrn-iris-probe.ko" | grep -E '^(vermagic|depends)'

# The toolchain's own linux/gpio.h predates the v2 API; take the host's (same guard).
aarch64-none-linux-gnu-gcc -include /usr/include/linux/gpio.h -O2 -Wall -Wextra -Werror -static -o "$HERE/fp-irq-probe" "$HERE/../fp-irq-probe.c"
cc -O2 -Wall -Wextra -Werror -o "$TMP/fp-irq-probe-host" "$HERE/../fp-irq-probe.c"
"$TMP/fp-irq-probe-host" --dry-run
