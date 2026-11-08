#!/bin/bash
# Build the optional x86_64 compatibility layer on an aarch64 Arch Linux ARM
# system (the phone itself, or a builder): box64, then ztos-x86-compat.
#
#   packages/ztos-x86-compat/build.sh [--install] [OUTDIR]
#
# Box64 is ~10 minutes of compiling on two Exynos cores; it runs at nice 19,
# with make -j2 unless MAKEFLAGS says otherwise. cmake is a build dependency
# only: it is installed as a dependency and can go afterwards with
# `pacman -Rns cmake`. --install installs both packages the way the App Store
# would (box64 as a dependency, ztos-x86-compat explicitly).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
install=0
[[ ${1:-} == --install ]] && { install=1; shift; }
out=${1:-$PWD/x86-compat-pkgs}
mkdir -p "$out"
export MAKEFLAGS=${MAKEFLAGS:--j2} PKGDEST=$out

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp -rL "$here/../box64" "$work/box64"
cp -rL "$here" "$work/ztos-x86-compat"

sudo pacman -S --needed --noconfirm --asdeps cmake python
(cd "$work/box64" && nice -n 19 makepkg -f --noconfirm)
(cd "$work/ztos-x86-compat" && makepkg -f --noconfirm -d)
ls -la "$out"

if ((install)); then
	sudo pacman -U --needed --noconfirm --asdeps "$out"/box64-*.pkg.tar.*
	sudo pacman -U --noconfirm "$out"/ztos-x86-compat-*.pkg.tar.*
fi
