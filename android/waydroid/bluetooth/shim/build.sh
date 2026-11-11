#!/bin/bash
# build.sh: build the shim, out/libbluetooth_jni.so (aarch64, Android 13).
# Run fetch.sh first. Everything comes from $ZTBT_WORK (default ~/ztbt-work).
#
# ABI: the shim shares C++ objects with the stock JNI code (std::vector,
# std::string, base::Callback, the interface vtables), so it is compiled
#  - with the NDK r25c clang, the compiler Android 13 is built with;
#  - against the platform's libc++ (namespace std::__1, the image's libc++.so),
#    not the NDK's (std::__ndk1);
#  - with NDEBUG and no DCHECKs, as the stock library: libchrome's
#    RefCountedThreadSafeBase has extra members when DCHECKs are on;
#  - with libchrome's callback_internal.cc and ref_counted.cc built in, the
#    same code the stock library has, acting on the same layout.
# After linking, checks: only JNI_OnLoad exported, only the libraries the
# APEX may load, nothing undefined, and sizeof(bt_interface_t) equal to the
# size of the stock bluetoothInterface symbol.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/sources.conf"
work=${ZTBT_WORK:-$HOME/ztbt-work}
out=${ZTBT_OUT:-$here/out}
bin=$work/$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin
cxx=$bin/aarch64-linux-android33-clang++
libs=$work/image-libs
[[ -x $cxx ]] || { echo "no NDK: run fetch.sh" >&2; exit 1; }
mkdir -p "$out/obj"

flags=(
	-std=c++17 -O2 -fPIC -DNDEBUG -D_LIBCPP_ABI_NAMESPACE=__1
	-fsanitize=shadow-call-stack -fno-exceptions -fno-rtti   # as AOSP builds arm64 platform code
	-fvisibility=hidden -fvisibility-inlines-hidden -ffunction-sections -fdata-sections
	-Wall -Wno-unused-parameter -Wno-missing-field-initializers -Wno-implicit-const-int-float-conversion
	-I"$here"
	-I"$work/src/bt/system/include" -I"$work/src/bt/system/include/hardware"
	-I"$work/src/bt/system/types" -I"$work/src/bt/system"
	-I"$work/src/libchrome"
)
srcs=(
	"$here/wire.cc" "$here/conn.cc" "$here/shim.cc" "$here/wrapper.cc"
	"$work/src/libchrome/base/callback_internal.cc" "$work/src/libchrome/base/memory/ref_counted.cc"
)
objs=()
for s in "${srcs[@]}"; do
	o=$out/obj/$(basename "${s%.cc}").o
	nice "$cxx" "${flags[@]}" -c "$s" -o "$o"
	objs+=("$o")
done

so=$out/libbluetooth_jni.so
nice "$cxx" -shared -nostdlib++ -o "$so" "${objs[@]}" \
	-Wl,-soname,libbluetooth_jni.so -Wl,--no-undefined -Wl,--gc-sections -Wl,-z,relro,-z,now \
	-Wl,--exclude-libs,ALL -L"$libs" -l:libc++.so -l:liblog.so -ldl
"$bin/llvm-strip" --strip-unneeded "$so"

# ---- checks --------------------------------------------------------------------
readelf=$bin/llvm-readelf
fail=0
exported=$("$readelf" --dyn-syms -W "$so" | awk '$5=="GLOBAL" && $7!="UND" {print $8}' | sort -u)
if [[ $exported != JNI_OnLoad ]]; then
	echo "exports more than JNI_OnLoad:"; echo "$exported" | head; fail=1
fi
needed=$("$readelf" -d "$so" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | sort | tr '\n' ' ')
for n in $needed; do
	case $n in libc++.so|liblog.so|libdl.so|libc.so|libm.so) ;; *) echo "needs $n"; fail=1 ;; esac
done
want=$("$readelf" --dyn-syms -W "$libs/libbluetooth_jni.so" | awk '$8=="bluetoothInterface" {print $3}')
cat > "$out/obj/size.cc" <<'EOF'
#include <hardware/bluetooth.h>
static_assert(sizeof(bt_interface_t) == STOCK_SIZE, "bt_interface_t differs from the stock bluetoothInterface");
EOF
if ! "$cxx" "${flags[@]}" -DSTOCK_SIZE=$((want)) -fsyntax-only "$out/obj/size.cc"; then
	echo "sizeof(bt_interface_t) is not $((want)), the stock bluetoothInterface's size"; fail=1
fi
"$here/abi-check.sh" "$bin/llvm-objdump" "$libs/libbluetooth_jni.so" \
	"$out/obj/callback_internal.o" "$out/obj/ref_counted.o" || fail=1
((fail == 0)) || { echo "build checks failed" >&2; exit 1; }
echo "built $so ($(stat -c %s "$so") bytes): exports JNI_OnLoad, needs $needed"
echo "  sizeof(bt_interface_t) = $((want)) = the stock bluetoothInterface"
sha256sum "$so"
