#!/bin/sh
# Put a module set from scripts/build-bt-modules.sh on the phone: as root, from
# the set's directory copied there (out/modules-<release>/ or its .tar).
#
#   sudo sh install-modules.sh            install into /usr/lib/modules/<release>/extra
#   sh install-modules.sh --verify        is the running kernel the one they are for?
#
# Install before flashing the kernel they are for, so its first boot finds them.
# Other releases' directories are not touched: a 7.1.0-zboot kernel keeps
# /usr/lib/modules/7.1.0-zboot, a 7.1.0-zboot-wd one gets its own. Also installs
# the loaders in sbin/ (they insmod from /usr/lib/modules/$(uname -r)/extra, and
# fall back to the old paths, so they work for either kernel).
set -eu
D=$(cd "$(dirname "$0")" && pwd)
REL=$(cat "$D/RELEASE")
ABI=$(cat "$D/ABI")
M=/usr/lib/modules/$REL

abi() {	# the running kernel's config, as scripts/module-abi.sh hashes it
	zcat /proc/config.gz | grep '^CONFIG_' |
		grep -vE '^CONFIG_(CMDLINE|INITRAMFS_[A-Z0-9_]+|EXTRA_FIRMWARE|EXTRA_FIRMWARE_DIR|PAHOLE_VERSION|RUSTC_VERSION|RUSTC_LLVM_VERSION|RUST_IS_AVAILABLE|BINDGEN_VERSION_TEXT)=' |
		LC_ALL=C sort | sha256sum | cut -d' ' -f1
}

if [ "${1:-}" = --verify ]; then
	[ "$(uname -r)" = "$REL" ] || { echo "running $(uname -r); these are for $REL"; exit 1; }
	[ "$(abi)" = "$ABI" ] || { echo "MISMATCH: $REL is running with another config than these modules'"; exit 1; }
	echo "ok: the running kernel's config is the one these modules were built against"
	exit 0
fi

[ "$(id -u)" = 0 ] || { echo "as root" >&2; exit 1; }
(cd "$D" && sha256sum -c --quiet SHA256SUMS) || { echo "the set is damaged" >&2; exit 1; }
for k in "$D"/*.ko; do
	v=$(modinfo -F vermagic "$k")
	case "$v" in
	"$REL "*) ;;
	*) echo "$k: vermagic '$v', not $REL" >&2; exit 1 ;;
	esac
done
if [ "$(uname -r)" = "$REL" ] && [ "$(abi)" != "$ABI" ]; then
	echo "the running $REL kernel has another config than these modules: not installing" >&2
	exit 1
fi
install -d "$M/extra"
install -m644 "$D"/*.ko "$M/extra/"
for f in modules.builtin modules.builtin.modinfo; do
	[ -f "$D/$f" ] && install -m644 "$D/$f" "$M/"
done
install -m644 "$D/ABI" "$M/extra/ztos-modules.abi"
depmod -a "$REL"
if [ -d "$D/sbin" ]; then
	install -m755 "$D"/sbin/* /usr/local/sbin/
fi
echo "installed $(ls "$D"/*.ko | wc -l) modules into $M/extra (ABI $ABI)"
ls /usr/lib/modules
