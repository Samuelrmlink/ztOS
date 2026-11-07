#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the ztpods-qt RPM in a Rocky Linux 10 container (podman, rootless), so it
# matches a Rocky/RHEL 10 host without any -devel packages installed there.
#   ztmetro/pods/qt/rpm.sh [IMAGE]     default quay.io/rockylinux/rockylinux:10
# Output: out/rpm/ztpods-qt-*.rpm (and the .src.rpm). Builds what is committed (HEAD).
set -euo pipefail
ROOT=$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)
IMAGE=${1:-quay.io/rockylinux/rockylinux:10}
SPEC=ztmetro/pods/qt/ztpods-qt.spec
VER=$(sed -n 's/^Version:\s*//p' "$ROOT/$SPEC")
COMMIT=$(git -C "$ROOT" rev-parse --short HEAD)
OUT=$ROOT/out/rpm
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/SOURCES" "$WORK/SPECS" "$OUT"
git -C "$ROOT" archive --prefix="ztpods-qt-$VER/" -o "$WORK/SOURCES/ztpods-qt-$VER.tar.gz" HEAD ztmetro/pods LICENSE
cp "$ROOT/$SPEC" "$WORK/SPECS/"
podman run --rm --security-opt label=disable -v "$WORK:/rpm" "$IMAGE" bash -euc "
	dnf -y -q install rpm-build dnf-plugins-core >/dev/null
	dnf -y -q builddep /rpm/SPECS/ztpods-qt.spec >/dev/null
	rpmbuild --define '_topdir /rpm' --define 'commit $COMMIT' -ba /rpm/SPECS/ztpods-qt.spec
"
cp "$WORK"/RPMS/*/*.rpm "$WORK"/SRPMS/*.rpm "$OUT/"
ls -l "$OUT"
