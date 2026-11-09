// SPDX-License-Identifier: GPL-2.0-or-later
// The shim's copy of ztos-btbridge's wire format. The table of record is
// ../ztbt/proto.py; tests/test_proto.py checks this file against it.
#pragma once

#include <stdint.h>

namespace ztbt {

constexpr uint32_t kVersion = 1;
constexpr size_t kMaxPacket = 65536;
constexpr uint16_t kFlagFd = 0x0001;   // header flag: one fd comes with the packet (SCM_RIGHTS)
constexpr const char* kSocketPath = "/data/misc/ztos-btbridge/bridge.sock";

enum Op : uint16_t {
  ZTBT_OP_REPLY = 0x0001,
  ZTBT_OP_HELLO = 0x0002,
  ZTBT_OP_ENABLE = 0x0003,
  ZTBT_OP_BONDED_LIST = 0x0010,
  ZTBT_OP_DEVICE_GET = 0x0011,
  ZTBT_OP_SCAN_START = 0x0020,
  ZTBT_OP_SCAN_STOP = 0x0021,
  ZTBT_OP_GATT_OPEN = 0x0030,
  ZTBT_OP_GATT_CLOSE = 0x0031,
  ZTBT_OP_GATT_DISCOVER = 0x0032,
  ZTBT_OP_GATT_READ = 0x0033,
  ZTBT_OP_GATT_WRITE = 0x0034,
  ZTBT_OP_GATT_MTU = 0x0035,
  ZTBT_OP_RSSI = 0x0036,
  ZTBT_OP_BOND_CREATE = 0x0040,
  ZTBT_OP_BOND_CANCEL = 0x0041,
  ZTBT_OP_BOND_REMOVE = 0x0042,
  ZTBT_OP_RFCOMM_CONNECT = 0x0050,   // carries one fd (kFlagFd): the bridge's end of the stream
  ZTBT_OP_EV_ADAPTER = 0x8001,
  ZTBT_OP_EV_DEVICE_FOUND = 0x8010,
  ZTBT_OP_EV_BOND_STATE = 0x8012,
  ZTBT_OP_EV_ACL = 0x8013,
  ZTBT_OP_EV_SCAN_STOPPED = 0x8020,
  ZTBT_OP_EV_LINK = 0x8030,
  ZTBT_OP_EV_GATT_VALUE = 0x8031,
};

// argument signatures (requests and events), and the replies' after the status
#define ZTBT_SIG_HELLO "us"
#define ZTBT_SIG_HELLO_REPLY "uasuBy"
#define ZTBT_SIG_ENABLE "B"
#define ZTBT_SIG_ENABLE_REPLY ""
#define ZTBT_SIG_BONDED_LIST ""
#define ZTBT_SIG_BONDED_LIST_REPLY "L(aysuyyBLU)"
#define ZTBT_SIG_DEVICE_GET "a"
#define ZTBT_SIG_DEVICE_GET_REPLY "T(aysuyyBLU)"
#define ZTBT_SIG_SCAN_START "y"
#define ZTBT_SIG_SCAN_START_REPLY ""
#define ZTBT_SIG_SCAN_STOP "y"
#define ZTBT_SIG_SCAN_STOP_REPLY ""
#define ZTBT_SIG_GATT_OPEN "ayy"
#define ZTBT_SIG_GATT_OPEN_REPLY "u"
#define ZTBT_SIG_GATT_CLOSE "u"
#define ZTBT_SIG_GATT_CLOSE_REPLY ""
#define ZTBT_SIG_GATT_DISCOVER "u"
#define ZTBT_SIG_GATT_DISCOVER_REPLY "L(yqqqUq)"
#define ZTBT_SIG_GATT_READ "uqq"
#define ZTBT_SIG_GATT_READ_REPLY "b"
#define ZTBT_SIG_GATT_WRITE "uqyqb"
#define ZTBT_SIG_GATT_WRITE_REPLY ""
#define ZTBT_SIG_GATT_MTU "u"
#define ZTBT_SIG_GATT_MTU_REPLY "q"
#define ZTBT_SIG_RSSI "a"
#define ZTBT_SIG_RSSI_REPLY "i"
#define ZTBT_SIG_BOND_CREATE "ay"
#define ZTBT_SIG_BOND_CREATE_REPLY ""
#define ZTBT_SIG_BOND_CANCEL "a"
#define ZTBT_SIG_BOND_CANCEL_REPLY ""
#define ZTBT_SIG_BOND_REMOVE "a"
#define ZTBT_SIG_BOND_REMOVE_REPLY ""
#define ZTBT_SIG_RFCOMM_CONNECT "aUiu"
#define ZTBT_SIG_RFCOMM_CONNECT_REPLY "u"
#define ZTBT_SIG_EV_ADAPTER "Bs"
#define ZTBT_SIG_EV_DEVICE_FOUND "T(aysuyyBLU)iib"
#define ZTBT_SIG_EV_BOND_STATE "ayu"
#define ZTBT_SIG_EV_ACL "aBy"
#define ZTBT_SIG_EV_SCAN_STOPPED "yu"
#define ZTBT_SIG_EV_LINK "uuBq"
#define ZTBT_SIG_EV_GATT_VALUE "uqb"

enum Status : uint32_t {
  ZTBT_ST_OK = 0,
  ZTBT_ST_FAIL = 1,
  ZTBT_ST_BUSY = 2,
  ZTBT_ST_NOT_FOUND = 3,
  ZTBT_ST_NOT_READY = 4,
  ZTBT_ST_UNSUPPORTED = 5,
  ZTBT_ST_AUTH_FAILED = 6,
  ZTBT_ST_NOT_PERMITTED = 7,
  ZTBT_ST_TIMEOUT = 8,
  ZTBT_ST_INVALID = 9,
  ZTBT_ST_IN_PROGRESS = 10,
  ZTBT_ST_CANCELLED = 11,
  ZTBT_ST_ATT = 12,
};

enum Transport : uint8_t { T_AUTO = 0, T_BREDR = 1, T_LE = 2 };
enum DevType : uint8_t { DEV_BREDR = 1, DEV_BLE = 2, DEV_DUAL = 3 };
enum Bond : uint8_t { BOND_NONE = 0, BOND_BONDING = 1, BOND_BONDED = 2 };
enum WriteType : uint8_t { W_NO_RSP = 1, W_DEFAULT = 2, W_PREPARE = 3 };

}  // namespace ztbt
