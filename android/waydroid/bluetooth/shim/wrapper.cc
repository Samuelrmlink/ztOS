// SPDX-License-Identifier: GPL-2.0-or-later
// libbluetooth_jni.so as Android's Bluetooth app loads it. The stock library
// (renamed libbluetooth_jni_stock.so in the same APEX directory) keeps its
// JNI code, which reaches the stack only through its exported
// bluetoothInterface. This copies the shim's bt_interface_t over that, then
// lets the stock JNI_OnLoad register its native methods. The stock stack is
// never initialised: its init and enable are no longer reachable.
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "log.h"
#include "shim.h"

namespace {

constexpr const char* kStock = "libbluetooth_jni_stock.so";

bool Overwrite(bt_interface_t* target) {
  long page = sysconf(_SC_PAGESIZE);
  uintptr_t start = (uintptr_t)target & ~(uintptr_t)(page - 1);
  uintptr_t end = ((uintptr_t)target + sizeof(*target) + page - 1) & ~(uintptr_t)(page - 1);
  // bluetoothInterface is const: it lies in RELRO, read-only after relocation
  if (mprotect((void*)start, end - start, PROT_READ | PROT_WRITE) != 0) {
    LOGE("mprotect: %s", strerror(errno));
    return false;
  }
  memcpy(target, &ztbt::kInterface, sizeof(*target));
  mprotect((void*)start, end - start, PROT_READ);
  return true;
}

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  void* stock = dlopen(kStock, RTLD_NOW | RTLD_LOCAL);
  if (!stock) {
    LOGE("can't load %s: %s", kStock, dlerror());
    return JNI_ERR;
  }
  auto* target = static_cast<bt_interface_t*>(dlsym(stock, "bluetoothInterface"));
  auto onload = reinterpret_cast<jint (*)(JavaVM*, void*)>(dlsym(stock, "JNI_OnLoad"));
  if (!target || !onload) {
    LOGE("%s: no bluetoothInterface or JNI_OnLoad", kStock);
    return JNI_ERR;
  }
  // the stock stack sets size to its own sizeof(bt_interface_t): a different
  // header version would be a different vtable of function pointers
  if (target->size != sizeof(bt_interface_t)) {
    LOGE("%s: bt_interface_t is %zu bytes, the shim's %zu: refusing", kStock, target->size,
         sizeof(bt_interface_t));
    return JNI_ERR;
  }
  if (!Overwrite(target)) return JNI_ERR;
  LOGI("bluetoothInterface now answers through ztos-btbridge");
  return onload(vm, reserved);
}
