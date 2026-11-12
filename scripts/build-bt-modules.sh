#!/bin/bash
# Build every module the S9 loads against a given kernel .config, so they
# match it: Bluetooth and its crypto, sound, the I2C bit-banging bus, and all
# of ztOS's own drivers (zrn-*). Matching vermagic is not enough: this kernel
# has no MODVERSIONS, so a module built for another config loads without
# complaint and then misreads kernel structures (power/BT-FIX.md; chg10 on
# 9 Nov: modules built without CONFIG_COMPAT oopsed 21 times on a COMPAT kernel).
#
#   scripts/build-bt-modules.sh <kernel .config> [output directory]
#
# The .config is the target kernel's: out/<image>.config from build-boot.sh (or
# build-boot.sh <name> ... --config-only, to build the modules before the image),
# or /proc/config.gz on the phone. The output directory defaults to
# out/modules-<release>/, which is where build-boot.sh looks for the set that goes
# with an image: the .ko files, RELEASE (uname -r of the kernel they are for),
# ABI (scripts/module-abi.sh of the config), SHA256SUMS, and install-modules.sh,
# which puts them on the phone (see its header). Also out/modules-<release>.tar.
#
# It needs a kernel tree with the port's drivers applied (build-boot.sh does it):
# build/linux-7.1, or KSRC=<tree>; and the Arm GNU 14.2 toolchain under
# build/toolchain. Built-ins stay as in the target kernel: only module symbols are
# added, and the build stops if any built-in would change. JOBS=n (default nproc).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CONFIG=$(realpath "${1:?kernel .config}")
SRC=$(realpath "${KSRC:-$ROOT/build/linux-7.1}")
TC=$ROOT/build/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin
export PATH=$TC:$PATH LC_ALL=C ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu-
BASE=out-target OB=out-target-bt OS=out-target-snd
JOBS=${JOBS:-$(nproc)}
ABI=$(sh "$ROOT/scripts/module-abi.sh" "$CONFIG")
RELEASE=$(sh "$ROOT/scripts/module-abi.sh" --release "$CONFIG" "$(make -s -C "$SRC" kernelversion)")
OUT=$(realpath -m "${2:-$ROOT/out/modules-$RELEASE}")
echo "modules for $RELEASE, config ABI $ABI"
cd "$SRC"

# Our drivers, built out of tree (M=), each in its own directory: the tree they
# are built in (bluetooth or sound) and the modules from each that ship.
EXT_BT="bluetooth/driver:zrn-bt sensors/driver:zrn-bbd leds:zrn-led leds:zrn-flash
	modem/zrn-modem:zrn-modem haptics:zrn-haptic power/hotplug:zrn-hotplug
	power/idle:zrn-cpuidle power/suspend:zrn-sleep-init power/rtc:zrn-exynos-rtc
	camera/driver:zrn-cam"
EXT_SND="audio/driver:snd-zrn-abox"

setup() {	# <dir>: the target's config, without paths from its own build
	mkdir -p "$1"
	cp "$CONFIG" "$1/.config"
	./scripts/config --file "$1/.config" --set-str INITRAMFS_SOURCE "" \
		--set-str EXTRA_FIRMWARE "" --set-str EXTRA_FIRMWARE_DIR ""
}
builtins() {	# <dir>: its =y symbols, but for the ones the module trees add
	grep -E '^CONFIG_[A-Z0-9_]+=y' "$1/.config" |
		grep -vE '^CONFIG_(BT(_[A-Z0-9_]+)?|CRYPTO_(ECDH|KPP|ECC|CMAC|AES)|SOUND|SND(_[A-Z0-9_]+)?)=' | sort
}
same_builtins() {	# <dir>: anything built in differently would change what the modules assume
	if ! diff <(builtins "$BASE") <(builtins "$1") > "$1/builtin.diff"; then
		echo "$1: built-ins changed compared with the target kernel:" >&2
		cat "$1/builtin.diff" >&2
		[ -n "${ALLOW_BUILTIN_DIFF:-}" ] || exit 1
	fi
}
quiet() { grep -E ' error |\*\*\*|undefined|ERROR' || true; }
# In-tree modules here have no vmlinux.o beside them, so modpost cannot see
# the kernel's exports and warns about every one; the check below the build
# (each shipped module's undefined symbols) takes the place of those warnings.
intree() { grep -E ' error |\*\*\*|ERROR' || true; }
symvers() {	# <tree>: Module.symvers = the kernel's exports + this tree's modules'
	cat "$BASE/vmlinux.symvers" "$1/modules-only.symvers" > "$1/Module.symvers"
}
external() {	# <tree> <list>: build (from clean) each directory of the list in that tree
	local d
	for d in $(printf '%s\n' $2 | cut -d: -f1 | sort -u); do
		# what another config's build left (not make clean: it deletes the
		# modem's committed zrn-modem-overlay.dtbo)
		find "$ROOT/$d" \( -name '*.o' -o -name '*.ko' -o -name '*.mod' -o -name '*.mod.c' \
			-o -name '.*.cmd' -o -name modules.order -o -name Module.symvers \) -delete
		make -s -j"$JOBS" O="$1" M="$ROOT/$d" modules 2>&1 | quiet
	done
}

echo "=== the target kernel ($BASE) ==="
# Always from the target's config: a tree left from another kernel's build is
# made again (incrementally), never reused as it is.
setup "$BASE"
make -s O="$BASE" olddefconfig
# olddefconfig changed nothing that matters: this tree and toolchain make the
# kernel the config came from
[ "$(sh "$ROOT/scripts/module-abi.sh" "$BASE/.config")" = "$ABI" ] || {
	echo "the config does not survive olddefconfig in $SRC unchanged (another tree or toolchain?)" >&2
	diff <(grep '^CONFIG_' "$CONFIG" | sort) <(grep '^CONFIG_' "$BASE/.config" | sort) | head -20 >&2
	exit 1
}
make -s -j"$JOBS" O="$BASE" vmlinux
[ -s "$BASE/vmlinux.symvers" ]
[ "$(cat "$BASE/include/config/kernel.release")" = "$RELEASE" ] || {
	echo "$SRC builds release $(cat "$BASE/include/config/kernel.release"), not $RELEASE" >&2
	exit 1
}

# The target's own =m drivers outside the trees below: the USB Ethernet ones
# (build-boot.sh --usb-net), built in the target tree itself
NET=
if grep -q '^CONFIG_USB_USBNET=m' "$CONFIG"; then
	echo "=== USB Ethernet, in the target tree ==="
	for m in USB_USBNET:drivers/net/usb/usbnet USB_NET_CDCETHER:drivers/net/usb/cdc_ether \
		USB_NET_CDC_NCM:drivers/net/usb/cdc_ncm USB_RTL8152:drivers/net/usb/r8152 MII:drivers/net/mii \
		USB_NET_AX88179_178A:drivers/net/usb/ax88179_178a USB_RTL8153_ECM:drivers/net/usb/r8153_ecm; do
		grep -q "^CONFIG_${m%%:*}=m" "$CONFIG" && NET="$NET ${m#*:}.ko"
	done
	make -s -j"$JOBS" O="$BASE" KBUILD_MODPOST_WARN=1 $NET 2>&1 | intree
fi
# and the HID ones (build-boot.sh --hid): HID itself is built in, these are =m
HIDM=
for m in UHID:drivers/hid/uhid HID_MULTITOUCH:drivers/hid/hid-multitouch \
	HID_LOGITECH:drivers/hid/hid-logitech HID_LOGITECH_DJ:drivers/hid/hid-logitech-dj \
	HID_LOGITECH_HIDPP:drivers/hid/hid-logitech-hidpp HID_APPLE:drivers/hid/hid-apple \
	HID_MICROSOFT:drivers/hid/hid-microsoft INPUT_FF_MEMLESS:drivers/input/ff-memless; do
	grep -q "^CONFIG_${m%%:*}=m" "$CONFIG" && HIDM="$HIDM ${m#*:}.ko"
done
if [ -n "$HIDM" ]; then
	echo "=== HID drivers, in the target tree ==="
	make -s -j"$JOBS" O="$BASE" KBUILD_MODPOST_WARN=1 $HIDM 2>&1 | intree
	NET="$NET $HIDM"
fi

echo "=== with Bluetooth as modules ($OB) ==="
python3 "$ROOT/bluetooth/patch-btbcm.py" "$SRC"
setup "$OB"
./scripts/config --file "$OB/.config" --module BT --module BT_HCIUART --enable SERIAL_DEV_BUS \
	--enable BT_HCIUART_SERDEV --enable BT_HCIUART_H4 --enable BT_HCIUART_BCM --module BT_BCM \
	--module BT_RFCOMM --enable BT_BREDR --enable BT_LE --module BT_HIDP \
	--module CRYPTO_ECDH --module CRYPTO_KPP --module CRYPTO_ECC --module CRYPTO_CMAC --module CRYPTO_AES
make -s O="$OB" olddefconfig
same_builtins "$OB"
make -s -j"$JOBS" O="$OB" modules_prepare
cp "$BASE/vmlinux.symvers" "$OB/vmlinux.symvers"
make -s -j"$JOBS" O="$OB" KBUILD_MODPOST_WARN=1 modules 2>&1 | intree
symvers "$OB"
external "$OB" "$EXT_BT"

echo "=== sound, on its own tree ($OS; see audio/build.sh) ==="
setup "$OS"
./scripts/config --file "$OS/.config" -m SOUND -m SND -m SND_DUMMY -e SND_DRIVERS \
	-d SND_SOC -d SND_USB -d SND_HRTIMER -d SND_ALOOP -d SND_OSSEMUL -d SND_SEQUENCER -d SND_PCI \
	-d SND_HDA -d SND_SPI -d SND_ARM -d SND_VIRTIO
make -s O="$OS" olddefconfig
same_builtins "$OS"
make -s -j"$JOBS" O="$OS" modules_prepare
cp "$BASE/vmlinux.symvers" "$OS/vmlinux.symvers"
make -s -j"$JOBS" O="$OS" KBUILD_MODPOST_WARN=1 sound/soundcore.ko sound/core/snd.ko sound/core/snd-timer.ko \
	sound/core/snd-pcm.ko drivers/i2c/algos/i2c-algo-bit.ko drivers/i2c/busses/i2c-gpio.ko 2>&1 | intree
symvers "$OS"
external "$OS" "$EXT_SND"

echo "=== the set ($OUT) ==="
rm -rf "$OUT"
mkdir -p "$OUT"
missing=
ship() { if [ -f "$1" ]; then cp "$1" "$OUT/"; else missing="$missing $1"; fi; }
for f in "$OS"/sound/soundcore.ko "$OS"/sound/core/snd{,-timer,-pcm}.ko \
	"$OS"/drivers/i2c/algos/i2c-algo-bit.ko "$OS"/drivers/i2c/busses/i2c-gpio.ko \
	"$OB"/crypto/{kpp,ecc,ecdh_generic,cmac,aes}.ko "$OB"/net/bluetooth/bluetooth.ko \
	"$OB"/net/bluetooth/rfcomm/rfcomm.ko "$OB"/drivers/bluetooth/{hci_uart,btbcm}.ko; do
	ship "$f"
done
for e in $EXT_BT $EXT_SND; do
	ship "$ROOT/${e%%:*}/${e#*:}.ko"
done
for f in $NET; do
	ship "$BASE/$f"
done
[ -z "$missing" ] || { echo "not built:$missing" >&2; exit 1; }
for k in "$OUT"/*.ko; do
	v=$(modinfo -F vermagic "$k" 2>/dev/null || strings "$k" | sed -n 's/^vermagic=//p')
	case "$v" in
	"$RELEASE "*) ;;
	*) echo "$k: vermagic '$v', not $RELEASE" >&2; exit 1 ;;
	esac
done
# every symbol each module needs is the kernel's or another module's of the set
T=$(mktemp -d)
mkdir -p "$T/lib/modules/$RELEASE/extra" "$T/usr/lib"
ln -s ../../lib/modules "$T/usr/lib/modules"	# wherever this kmod looks
cp "$OUT"/*.ko "$T/lib/modules/$RELEASE/extra/"
unknown=$(depmod -e -F "$BASE/System.map" -b "$T" "$RELEASE" 2>&1 | grep -i 'unknown symbol' || true)
rm -rf "$T"
[ -z "$unknown" ] || { echo "$unknown" >&2; echo "modules need symbols nothing provides" >&2; exit 1; }
echo "$RELEASE" > "$OUT/RELEASE"
echo "$ABI" > "$OUT/ABI"
cp "$CONFIG" "$OUT/config"
for f in modules.builtin modules.builtin.modinfo; do
	[ -f "$BASE/$f" ] && cp "$BASE/$f" "$OUT/"
done
install -m755 "$ROOT/devices/samsung-starlte/install-modules.sh" "$OUT/install-modules.sh"
# the loaders that insmod by path, which look in /usr/lib/modules/$(uname -r) now
mkdir -p "$OUT/sbin"
for f in ztoscorenetworking-modem zrn-rtc-load zrn-sleepd; do
	install -m755 "$ROOT/devices/samsung-starlte/overlay/usr/local/sbin/$f" "$OUT/sbin/"
done
(cd "$OUT" && sha256sum *.ko > SHA256SUMS)
tar cf "$OUT.tar" -C "$(dirname "$OUT")" "$(basename "$OUT")"
ls -la "$OUT"
echo "MODULES_OK $OUT ($(ls "$OUT"/*.ko | wc -l) modules, $RELEASE, ABI $ABI)"
