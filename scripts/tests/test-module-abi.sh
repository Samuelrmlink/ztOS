#!/bin/bash
# scripts/module-abi.sh: what counts as a different module ABI. No kernel.
#
#   scripts/tests/test-module-abi.sh
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ABI=$ROOT/scripts/module-abi.sh
fails=0
ok() { echo "ok    $1"; }
bad() { echo "FAIL  $1"; fails=$((fails + 1)); }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cat > "$T/a" <<'EOF'
CONFIG_LOCALVERSION="-zboot"
CONFIG_CMDLINE="console=tty0"
CONFIG_INITRAMFS_SOURCE="/build/initramfs.list"
CONFIG_INITRAMFS_ROOT_UID=0
CONFIG_EXTRA_FIRMWARE="brcm/x.bin"
CONFIG_EXTRA_FIRMWARE_DIR="/build/firmware"
CONFIG_BT=m
# CONFIG_COMPAT is not set
EOF
h() { sh "$ABI" "$1"; }
a=$(h "$T/a")

sed -e 's/console=tty0/console=tty0 x=1/' -e 's|"/build/|"/elsewhere/|' -e '/INITRAMFS_ROOT_UID/d' \
	-e 's|brcm/x.bin||' "$T/a" > "$T/b"
[ "$(h "$T/b")" = "$a" ] && ok "command line, initramfs and firmware lists: the same ABI" || bad "paths changed the ABI"

printf 'CONFIG_COMPAT=y\n' >> "$T/b"
[ "$(h "$T/b")" != "$a" ] && ok "CONFIG_COMPAT: another ABI" || bad "COMPAT not seen"

sed 's/-zboot"/-zboot-wd"/' "$T/a" > "$T/c"
[ "$(h "$T/c")" != "$a" ] && ok "another release: another ABI" || bad "release not seen"
[ "$(sh "$ABI" --release "$T/c" 7.1.0)" = 7.1.0-zboot-wd ] && ok "release from LOCALVERSION" || bad "release"

sort -r "$T/a" > "$T/d"
[ "$(h "$T/d")" = "$a" ] && ok "order does not matter" || bad "order"

[ "$fails" = 0 ] && echo "all passed" || { echo "$fails failed"; exit 1; }
