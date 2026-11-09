#!/bin/bash
# host-build.sh: the shim and tests/host_test.cc for the PC (g++), for
# test_shim_host.py. Same sources and headers as build.sh; the PC's libstdc++,
# so nothing here says anything about the Android ABI (build.sh checks that).
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
work=${ZTBT_WORK:-$HOME/ztbt-work}
out=${ZTBT_HOST_OUT:-$here/out-host}
mkdir -p "$out"
[[ -d $work/src/bt/system/include && -d $work/src/libchrome/base ]] || { echo "run fetch.sh first" >&2; exit 1; }
nice g++ -std=c++17 -O1 -g -DNDEBUG -pthread -Wall -Wno-unused-parameter -Wno-missing-field-initializers \
	-Wno-deprecated-declarations -Wno-attributes -Wno-unused-function \
	-I"$here" -I"$work/src/bt/system/include" -I"$work/src/bt/system/include/hardware" \
	-I"$work/src/bt/system/types" -I"$work/src/bt/system" -I"$work/src/libchrome" \
	"$here/wire.cc" "$here/conn.cc" "$here/shim.cc" "$here/tests/host_test.cc" \
	"$work/src/libchrome/base/callback_internal.cc" "$work/src/libchrome/base/memory/ref_counted.cc" \
	-o "$out/host_test"
echo "$out/host_test"
