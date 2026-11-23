#!/bin/sh
# Run on the phone, from ztmetro/ in the private swarm-camera copy.
set -eu
make camera/ztcamera
check_dir=$(mktemp -d /tmp/ztcamera-check-build-XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
# the Makefile's own compiler line, core sources and libraries for the apps,
# so this keeps building when the core gains a protocol or a library
mk() { make -s --no-print-directory --eval "pv-$1:; @echo \$($1)" "pv-$1"; }
$(mk APP_BUILD) -Wextra -o "$check_dir/test-camera" camera/test-camera.c $(mk CORE) $(mk APP_LIBS)
"$check_dir/test-camera"
# Isolate render-mode camera-roll directories from the real user's captures.
mkdir -p "$check_dir/home"
mkdir -p camera/evidence
HOME="$check_dir/home" camera/ztcamera --render camera/evidence/reference.png
for dial in 0 1 2 3 4; do
    HOME="$check_dir/home" camera/ztcamera --render "camera/evidence/dial-$dial.png" dial "$dial"
done
if HOME="$check_dir/home" camera/ztcamera --render "$check_dir/invalid.png" dial 5; then
    echo 'invalid dial unexpectedly accepted' >&2
    exit 1
fi
printf '%s\n' 'Offline references: camera/evidence/*.png. Live phone screenshots still required.'
