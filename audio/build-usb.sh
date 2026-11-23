#!/bin/bash
# audio/build-usb.sh: USB audio (snd-usb-audio and what it needs) as modules
# for the flashed kernel boot-aod1, 7.1.0-zboot-wd-dp6 #63, into
# audio/modules-usb/. The running kernel image is not changed.
#
# The sound core on the phone (soundcore, snd, snd-timer, snd-pcm) was built
# in out-target-snd: boot-aod1's configuration with the sound core added as
# modules. This copies that tree to out-target-usbsnd, adds SND_USB_AUDIO (and
# through it SND_HWDEP, SND_RAWMIDI) as modules, and builds them. As a check
# that the extra options leave the core's structures alone, it rebuilds the
# core there too and compares it byte for byte with out-target-snd's, which are
# the files the phone has loaded. A difference stops the build.
#
#   audio/build-usb.sh [KERNEL_TREE]    default /home/samuel/ztos-aod-build/linux
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
K=${1:-/home/samuel/ztos-aod-build/linux}
TC=${ZT_TOOLCHAIN:-/home/samuel/git-clone/ztOS/build/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin}
export PATH=$TC:$PATH
export LC_ALL=C ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- KBUILD_MODPOST_WARN=1
BASE=out-target-snd OUT=out-target-usbsnd REL=7.1.0-zboot-wd-dp6
DEST=$ROOT/audio/modules-usb

[ "$(cat "$K/$BASE/include/config/kernel.release")" = $REL ] || { echo "$K/$BASE is not $REL"; exit 1; }
# Only the sound options may differ between boot-aod1's own build (out/) and
# the sound tree: the rest of the configuration has to be the flashed kernel's.
odd=$(diff <(grep '^CONFIG_' "$K/out/.config" | grep -v -e '^CONFIG_SND' -e '^CONFIG_SOUND' \
		-e '^CONFIG_EXTRA_FIRMWARE' -e '^CONFIG_INITRAMFS' | sort) \
	<(grep '^CONFIG_' "$K/$BASE/.config" | grep -v -e '^CONFIG_SND' -e '^CONFIG_SOUND' \
		-e '^CONFIG_EXTRA_FIRMWARE' -e '^CONFIG_INITRAMFS' | sort) || true)
[ -z "$odd" ] || { echo "$BASE's configuration differs from out/ beyond sound:"; echo "$odd"; exit 1; }

if [ ! -f "$K/$OUT/.config" ] || [ "$K/$BASE/.config" -nt "$K/$OUT/.config" ]; then
	rm -rf "${K:?}/$OUT"
	cp -a "$K/$BASE" "$K/$OUT"
	(cd "$K" && scripts/config --file $OUT/.config -e SND_USB -m SND_USB_AUDIO -d SND_USB_AUDIO_MIDI_V2 \
		-d SND_USB_UA101 -d SND_USB_CAIAQ -d SND_USB_6FIRE -d SND_USB_HIFACE -d SND_BCD2000 \
		-d SND_USB_POD -d SND_USB_PODHD -d SND_USB_TONEPORT -d SND_USB_VARIAX)
	make -s -C "$K" O=$OUT olddefconfig
fi
echo "== configuration added over $BASE"
diff <(grep '^CONFIG_' "$K/$BASE/.config" | sort) <(grep '^CONFIG_' "$K/$OUT/.config" | sort) | grep '^[<>]' || true
# PAHOLE_VERSION and RUSTC_* are what Kconfig finds on the build host (the
# kernel was built in a container with pahole and without rustc); BTF and Rust
# are off, and the core comparison below shows whether anything noticed.
odd=$(diff <(grep '^CONFIG_' "$K/$BASE/.config" | grep -v -e '^CONFIG_SND' -e '^CONFIG_PAHOLE_VERSION=' -e '^CONFIG_RUSTC_' | sort) \
	<(grep '^CONFIG_' "$K/$OUT/.config" | grep -v -e '^CONFIG_SND' -e '^CONFIG_PAHOLE_VERSION=' -e '^CONFIG_RUSTC_' | sort) | grep '^[<>]' || true)
[ -z "$odd" ] || { echo "the USB audio options changed something outside sound:"; echo "$odd"; exit 1; }
odd=$(diff <(grep '^CONFIG_[A-Z0-9_]*=y' "$K/$BASE/.config" | sort) <(grep '^CONFIG_[A-Z0-9_]*=y' "$K/$OUT/.config" | sort) |
	grep '^[<>]' | grep -v -e 'CONFIG_SND_USB=y' || true)
[ -z "$odd" ] || echo "note: booleans changed: $odd"

CORE="sound/soundcore.ko sound/core/snd.ko sound/core/snd-timer.ko sound/core/snd-pcm.ko"
NEW="sound/core/snd-hwdep.ko sound/core/snd-rawmidi.ko sound/usb/snd-usbmidi-lib.ko sound/usb/snd-usb-audio.ko"
# Built as single targets, as audio/build.sh does; the core again so it can be compared.
make -j"$(nproc)" -C "$K" O=$OUT $CORE $NEW 2>&1 | grep -vE '^make\[|^  (CC|LD|AR|MODPOST|CHK|UPD|SYNC|GEN|CALL|DESCEND)' || true

echo "== core rebuilt with USB audio enabled vs the modules the phone runs ($BASE)"
for m in $CORE; do
	a=$(sha256sum < "$K/$BASE/$m" | cut -c1-64) b=$(sha256sum < "$K/$OUT/$m" | cut -c1-64)
	if [ "$a" = "$b" ]; then echo "same      $m $a"; else echo "DIFFERENT $m $a $b"; bad=1; fi
done
[ -z "${bad:-}" ] || { echo "the sound core would change: not installing the USB modules over it"; exit 1; }

mkdir -p "$DEST"
rm -f "$DEST"/*.ko
for m in $NEW; do cp "$K/$OUT/$m" "$DEST/"; done
echo "== $DEST"
for f in "$DEST"/*.ko; do
	vm=$(strings "$f" | sed -n 's/^vermagic=//p') dep=$(strings "$f" | sed -n 's/^depends=//p')
	printf '%s  %s\n    vermagic=%s\n    depends=%s\n' "$(sha256sum < "$f" | cut -c1-64)" "$(basename "$f")" "$vm" "$dep"
	[ "$vm" = "$REL SMP preempt mod_unload aarch64" ] || { echo "wrong vermagic"; exit 1; }
done
# Every symbol a new module needs comes from the kernel, the core or another new module.
need=$(for f in "$DEST"/*.ko; do aarch64-none-linux-gnu-nm -u "$f" | awk '{print $2}'; done | sort -u)
have=$( { awk '{print $2}' "$K/out/vmlinux.symvers"
	for m in $CORE; do aarch64-none-linux-gnu-nm --defined-only "$K/$OUT/$m" | awk '$2 ~ /[TDRB]/ {print $3}'; done
	for f in "$DEST"/*.ko; do aarch64-none-linux-gnu-nm --defined-only "$f" | awk '$2 ~ /[TDRB]/ {print $3}'; done; } | sort -u)
missing=$(comm -23 <(echo "$need") <(echo "$have") | grep -v -e '^__this_module$' -e '^_GLOBAL_OFFSET_TABLE_$' || true)
if [ -n "$missing" ]; then echo "unresolved symbols:"; echo "$missing"; exit 1; fi
echo "all symbols resolve against boot-aod1 (out/vmlinux.symvers) and the sound core"
