// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <hardware/bluetooth.h>

namespace ztbt {

// The shim's bt_interface_t: the wrapper copies it over the stock
// library's bluetoothInterface.
extern const bt_interface_t kInterface;

}  // namespace ztbt
