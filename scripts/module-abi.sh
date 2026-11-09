#!/bin/sh
# What a loadable module has to match in the kernel it is loaded into.
#
#   scripts/module-abi.sh CONFIG             the config's module ABI: a sha256
#   scripts/module-abi.sh --release CONFIG KERNELVERSION
#                                            the release (uname -r) it builds
#
# This kernel has no MODVERSIONS: a module built against another .config loads
# as long as the release string matches, and then reads kernel structures with
# the wrong layout. CONFIG_COMPAT is the case that bit (struct proto_ops gains
# compat_ioctl; chg10, 9 Nov: bt_sock_create oopsed in kmem_cache_alloc). So
# modules carry the hash of the .config they were built against
# (scripts/build-bt-modules.sh writes it to <set>/ABI), and build-boot.sh
# refuses an image whose module set has another one.
#
# The hash is over every CONFIG_ line except those that are paths or text and
# change no code a module sees: the command line, the initramfs and firmware
# lists, and host tool versions (pahole, rustc) that differ between a laptop
# and the build container. Takes a .config or /proc/config.gz (zcat it first).
set -eu
if [ "${1:-}" = --release ]; then
	cfg=${2:?config}
	ver=${3:?kernel version, as make kernelversion prints it}
	lv=$(sed -n 's/^CONFIG_LOCALVERSION="\(.*\)"$/\1/p' "$cfg")
	echo "$ver$lv"
	exit 0
fi
cfg=${1:?config}
grep '^CONFIG_' "$cfg" |
	grep -vE '^CONFIG_(CMDLINE|INITRAMFS_[A-Z0-9_]+|EXTRA_FIRMWARE|EXTRA_FIRMWARE_DIR|PAHOLE_VERSION|RUSTC_VERSION|RUSTC_LLVM_VERSION|RUST_IS_AVAILABLE|BINDGEN_VERSION_TEXT)=' |
	LC_ALL=C sort | sha256sum | cut -d' ' -f1
