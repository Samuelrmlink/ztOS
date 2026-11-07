#!/bin/bash
# Build the Bluetooth, crypto and hardware modules against a given kernel, so
# they match it. Matching vermagic is not enough: this kernel has no
# MODVERSIONS, so a module built for another kernel loads without complaint and
# then misreads kernel structures (see power/BT-FIX.md).
#
#   scripts/build-bt-modules.sh <kernel .config> <output directory>
#
# The .config is the target kernel's (/proc/config.gz on the device). It needs
# build/linux-7.1, with the port's drivers applied, and the Arm GNU 14.2
# toolchain under build/toolchain. Built-ins stay as in the target kernel: only
# module symbols are added.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CONFIG=$(realpath "${1:?kernel .config}")
OUT=$(realpath -m "${2:?output directory}")
SRC=$ROOT/build/linux-7.1
TC=$ROOT/build/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin
export PATH=$TC:$PATH LC_ALL=C ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu-
BASE=out-target OB=out-target-bt
cd "$SRC"
JOBS=${JOBS:-$(nproc)}

setup() {	# <dir>: the target's config, without paths from its own build
	mkdir -p "$1"
	cp "$CONFIG" "$1/.config"
	./scripts/config --file "$1/.config" --set-str INITRAMFS_SOURCE "" \
		--set-str EXTRA_FIRMWARE "" --set-str EXTRA_FIRMWARE_DIR ""
}

echo "=== the target kernel ==="
setup "$BASE"
make -s O="$BASE" olddefconfig
[ -s "$BASE/vmlinux.symvers" ] && [ -s "$BASE/Module.symvers" ] ||
	make -s -j"$JOBS" O="$BASE" vmlinux modules 2>/dev/null
[ -s "$BASE/Module.symvers" ]

echo "=== with Bluetooth as modules ==="
python3 "$ROOT/bluetooth/patch-btbcm.py" "$SRC"
setup "$OB"
./scripts/config --file "$OB/.config" --module BT --module BT_HCIUART --enable SERIAL_DEV_BUS \
	--enable BT_HCIUART_SERDEV --enable BT_HCIUART_H4 --enable BT_HCIUART_BCM --module BT_BCM \
	--module BT_RFCOMM --enable BT_BREDR --enable BT_LE --module BT_HIDP \
	--module CRYPTO_ECDH --module CRYPTO_KPP --module CRYPTO_ECC --module CRYPTO_CMAC --module CRYPTO_AES
make -s O="$OB" olddefconfig
make -s -j"$JOBS" O="$OB" modules_prepare
cp "$BASE/Module.symvers" "$OB/Module.symvers"
# anything built in differently would change what the modules assume
if ! diff <(grep -E '^CONFIG_[A-Z0-9_]+=y' "$BASE/.config" | grep -vE 'CONFIG_(BT|CRYPTO_(ECDH|KPP|ECC|CMAC|AES))' | sort) \
	<(grep -E '^CONFIG_[A-Z0-9_]+=y' "$OB/.config" | grep -vE 'CONFIG_(BT|CRYPTO_(ECDH|KPP|ECC|CMAC|AES))' | sort) > "$OB/builtin.diff"; then
	echo "built-ins changed compared with the target kernel:" >&2
	cat "$OB/builtin.diff" >&2
	[ -n "${ALLOW_BUILTIN_DIFF:-}" ] || exit 1
fi

echo "=== modules ==="
cp "$BASE/vmlinux.symvers" "$OB/vmlinux.symvers"
make -s -j"$JOBS" O="$OB" KBUILD_MODPOST_WARN=1 modules 2>&1 | grep -E ' error |\*\*\*|undefined' || true
for d in "$ROOT/bluetooth/driver" "$ROOT/sensors/driver" "$ROOT/leds"; do
	make -s -j"$JOBS" O="$OB" M="$d" modules 2>&1 |
		grep -E ' error |\*\*\*|undefined' || true
done

echo "=== sound, on its own tree (see audio/build.sh) ==="
OS=out-target-snd
setup "$OS"
./scripts/config --file "$OS/.config" -m SOUND -m SND -m SND_DUMMY -e SND_DRIVERS \
	-d SND_SOC -d SND_USB -d SND_HRTIMER -d SND_ALOOP -d SND_OSSEMUL -d SND_SEQUENCER -d SND_PCI \
	-d SND_HDA -d SND_SPI -d SND_ARM -d SND_VIRTIO
make -s O="$OS" olddefconfig
make -s -j"$JOBS" O="$OS" modules_prepare
cp "$BASE/Module.symvers" "$OS/Module.symvers"
cp "$BASE/vmlinux.symvers" "$OS/vmlinux.symvers"
make -s -j"$JOBS" O="$OS" KBUILD_MODPOST_WARN=1 sound/soundcore.ko sound/core/snd.ko sound/core/snd-timer.ko \
	sound/core/snd-pcm.ko drivers/i2c/algos/i2c-algo-bit.ko drivers/i2c/busses/i2c-gpio.ko 2>&1 |
	grep -E ' error |\*\*\*' || true
make -s O="$OS" KBUILD_MODPOST_WARN=1 M="$ROOT/audio/driver" modules 2>&1 | grep -E ' error |\*\*\*' || true

mkdir -p "$OUT"
for f in "$OS"/sound/soundcore.ko "$OS"/sound/core/snd{,-timer,-pcm}.ko \
	"$OS"/drivers/i2c/algos/i2c-algo-bit.ko "$OS"/drivers/i2c/busses/i2c-gpio.ko "$ROOT"/audio/driver/snd-zrn-abox.ko; do
	if [ -f "$f" ]; then cp "$f" "$OUT/"; else echo "(not built: $f)"; fi
done
for f in "$OB"/crypto/{kpp,ecc,ecdh_generic,cmac,aes}.ko "$OB"/net/bluetooth/bluetooth.ko \
	"$OB"/net/bluetooth/rfcomm/rfcomm.ko "$OB"/drivers/bluetooth/{hci_uart,btbcm}.ko \
	"$ROOT"/bluetooth/driver/zrn-bt.ko "$ROOT"/sensors/driver/zrn-bbd.ko "$ROOT"/leds/zrn-flash.ko; do
	if [ -f "$f" ]; then cp "$f" "$OUT/"; else echo "(not built: $f)"; fi
done
cp "$BASE/Module.symvers" "$OUT/Module.symvers"
ls -la "$OUT"
