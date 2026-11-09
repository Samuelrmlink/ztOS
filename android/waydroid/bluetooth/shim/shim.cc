// SPDX-License-Identifier: GPL-2.0-or-later
// Android 13's Bluetooth stack interfaces (bt_interface_t, btgatt_*,
// BleScannerInterface, BleAdvertiserInterface), answered by ztos-btbridge on
// the host, which uses BlueZ. See docs/waydroid.md, "Bluetooth for Android
// apps". Stage 1: adapter, bonded devices, LE scan and discovery, GATT client,
// pairing through BlueZ. The advertiser and GATT server refuse (stage 2);
// sockets are absent (stage 3).
//
// What this never does: talk HCI (dut_mode_send, le_test_mode refuse), hold
// or pass link keys, or ask Android for a PIN or a confirmation (pin_request
// and ssp_request are never called: BlueZ's agent and ztOS's sheet ask).
//
// Threads: Android calls in on its binder and JNI threads; every callback
// into Android runs on the Conn loop thread, which announced itself with
// ASSOCIATE_JVM. State below is owned by the loop thread; calls from other
// threads Post() to it. The few answers Android needs at once (device type,
// connection state) come from caches under cache_mu.
#include <stdlib.h>
#include <string.h>

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <base/callback.h>
#include <hardware/bluetooth.h>
#include <hardware/bt_gatt.h>

#include "conn.h"
#include "log.h"
#include "shim.h"
#include "ztbt_proto.h"

using bluetooth::Uuid;

namespace ztbt {
namespace {

static_assert(sizeof(Uuid) == 16, "bluetooth::Uuid layout");
static_assert(sizeof(RawAddress) == 6, "RawAddress layout");

constexpr int kGattSuccess = 0;
constexpr int kGattError = 0x85;      // Android's "133"
constexpr int kGattReqNotSupported = 0x06;
constexpr int kDiscoveryMs = 12800;   // what Android's inquiry lasts

Uuid UuidFrom(const std::array<uint8_t, 16>& be) { return Uuid::From128BitBE(be); }

RawAddress AddrFrom(const Value& v) {
  RawAddress a;
  v.addr(a.address);
  return a;
}

std::string Key(const RawAddress& a) { return AddrText(a.address); }

int GattStatus(uint32_t st, const Values& v) {
  if (st == ZTBT_ST_OK) return kGattSuccess;
  if (st == ZTBT_ST_ATT && !v.empty()) return v[0].num();
  if (st == ZTBT_ST_UNSUPPORTED) return kGattReqNotSupported;
  return kGattError;
}

bt_status_t BtStatus(uint32_t st) {
  switch (st) {
    case ZTBT_ST_OK: return BT_STATUS_SUCCESS;
    case ZTBT_ST_BUSY: case ZTBT_ST_IN_PROGRESS: return BT_STATUS_BUSY;
    case ZTBT_ST_NOT_READY: return BT_STATUS_NOT_READY;
    case ZTBT_ST_UNSUPPORTED: return BT_STATUS_UNSUPPORTED;
    case ZTBT_ST_AUTH_FAILED: case ZTBT_ST_TIMEOUT: return BT_STATUS_AUTH_FAILURE;
    case ZTBT_ST_CANCELLED: case ZTBT_ST_NOT_PERMITTED: return BT_STATUS_AUTH_REJECTED;
    case ZTBT_ST_INVALID: return BT_STATUS_PARM_INVALID;
    default: return BT_STATUS_FAIL;
  }
}

// A device as the bridge described it (DEV tuple).
struct Dev {
  RawAddress addr;
  uint8_t addr_type = 0;
  std::string name;
  uint32_t cls = 0;
  uint8_t type = DEV_BLE;
  uint8_t bond = BOND_NONE;
  bool connected = false;
  std::vector<std::array<uint8_t, 16>> uuids;
};

Dev DevFrom(const Value& t) {
  Dev d;
  const auto& f = t.items;
  d.addr = AddrFrom(f[0]);
  d.addr_type = (uint8_t)f[1].num();
  d.name = f[2].s;
  d.cls = f[3].num();
  d.type = (uint8_t)f[4].num();
  d.bond = (uint8_t)f[5].num();
  d.connected = f[6].flag();
  for (auto& u : f[7].items) d.uuids.push_back(u.uuid());
  return d;
}

// bt_property_t arrays need their values to live as long as the call.
class Props {
 public:
  template <typename T>
  void Add(bt_property_type_t type, const T& v) { AddRaw(type, &v, sizeof v); }
  void AddRaw(bt_property_type_t type, const void* p, size_t n) {
    store_.emplace_back((const char*)p, (const char*)p + n);
    types_.push_back(type);
  }
  void AddName(bt_property_type_t type, const std::string& name) {
    std::string n = name.substr(0, 248);
    AddRaw(type, n.data(), n.size());
  }
  bt_property_t* Get() {
    props_.clear();
    for (size_t i = 0; i < types_.size(); i++)
      props_.push_back(bt_property_t{types_[i], (int)store_[i].size(), store_[i].empty() ? nullptr : store_[i].data()});
    return props_.data();
  }
  int Count() const { return (int)types_.size(); }

 private:
  std::vector<std::vector<char>> store_;
  std::vector<bt_property_type_t> types_;
  std::vector<bt_property_t> props_;
};

void DeviceProps(const Dev& d, Props* p, bool with_rssi = false, int8_t rssi = 0) {
  p->Add(BT_PROPERTY_BDADDR, d.addr);
  if (!d.name.empty()) p->AddName(BT_PROPERTY_BDNAME, d.name);
  p->Add(BT_PROPERTY_CLASS_OF_DEVICE, d.cls);
  bt_device_type_t t = (bt_device_type_t)d.type;
  p->Add(BT_PROPERTY_TYPE_OF_DEVICE, t);
  std::vector<Uuid> us;
  for (auto& u : d.uuids) us.push_back(UuidFrom(u));
  p->AddRaw(BT_PROPERTY_UUIDS, us.data(), us.size() * sizeof(Uuid));
  if (with_rssi) p->Add(BT_PROPERTY_REMOTE_RSSI, rssi);
}

struct Link {
  int client_if = 0;
  RawAddress addr;
  bool open = false;
  std::vector<btgatt_db_element_t> db;
};

class Scanner;
class Advertiser;

struct State {
  Conn* conn = nullptr;
  bt_callbacks_t* cb = nullptr;
  const btgatt_callbacks_t* gcb = nullptr;
  ScanningCallbacks* scan_cb = nullptr;
  AdvertisingCallbacks* adv_cb = nullptr;

  // adapter
  bool hello = false;
  bool want_on = false;
  bool on = false;          // reported BT_STATE_ON
  bool bridge_on = false;   // ENABLE(true) answered on this connection
  RawAddress address;
  std::string name = "ztOS";
  uint32_t cls = 0;

  // discovery and scanning
  bool le_scan = false;
  bool discovery = false;
  uint64_t discovery_timer = 0;
  std::set<std::string> found;

  // GATT client
  int next_client = 1;
  std::map<int, Uuid> clients;
  std::map<uint32_t, Link> links;

  // read from Android's threads
  std::mutex cache_mu;
  std::map<std::string, Dev> devs;
  std::set<std::string> acl;
};

State* g = nullptr;

void Remember(const Dev& d) {
  std::lock_guard<std::mutex> l(g->cache_mu);
  g->devs[Key(d.addr)] = d;
}

// ---- adapter ---------------------------------------------------------------

void ReportAdapter(int which = 0) {
  if (!g->cb) return;
  Props p;
  bool all = which == 0;
  if (all || which == BT_PROPERTY_BDADDR) p.Add(BT_PROPERTY_BDADDR, g->address);
  if (all || which == BT_PROPERTY_BDNAME) p.AddName(BT_PROPERTY_BDNAME, g->name);
  if (all || which == BT_PROPERTY_CLASS_OF_DEVICE) p.Add(BT_PROPERTY_CLASS_OF_DEVICE, g->cls);
  if (all || which == BT_PROPERTY_TYPE_OF_DEVICE) {
    bt_device_type_t t = BT_DEVICE_DEVTYPE_DUAL;
    p.Add(BT_PROPERTY_TYPE_OF_DEVICE, t);
  }
  if (all || which == BT_PROPERTY_ADAPTER_SCAN_MODE) {
    bt_scan_mode_t m = BT_SCAN_MODE_CONNECTABLE;   // discoverable is the shell's to decide
    p.Add(BT_PROPERTY_ADAPTER_SCAN_MODE, m);
  }
  if (all || which == BT_PROPERTY_ADAPTER_DISCOVERABLE_TIMEOUT) {
    uint32_t t = 120;
    p.Add(BT_PROPERTY_ADAPTER_DISCOVERABLE_TIMEOUT, t);
  }
  if (all || which == BT_PROPERTY_UUIDS) p.AddRaw(BT_PROPERTY_UUIDS, nullptr, 0);
  if (all || which == BT_PROPERTY_LOCAL_LE_FEATURES) {
    bt_local_le_features_t f;
    memset(&f, 0, sizeof f);
    f.version_supported = 0x0060;
    f.local_privacy_enabled = 1;
    f.max_adv_instance = 0;          // stage 2 brings advertising
    f.max_adv_filter_supported = 0;  // Android filters scan results in software
    f.scan_result_storage_size = 0;  // no batch scans
    f.le_maximum_advertising_data_length = 31;
    p.Add(BT_PROPERTY_LOCAL_LE_FEATURES, f);
  }
  if (all || which == BT_PROPERTY_LOCAL_IO_CAPS || which == BT_PROPERTY_LOCAL_IO_CAPS_BLE) {
    bt_io_cap_t c = BT_IO_CAP_KBDISP;
    if (all || which == BT_PROPERTY_LOCAL_IO_CAPS) p.Add(BT_PROPERTY_LOCAL_IO_CAPS, c);
    if (all || which == BT_PROPERTY_LOCAL_IO_CAPS_BLE) p.Add(BT_PROPERTY_LOCAL_IO_CAPS_BLE, c);
  }
  if (p.Count()) g->cb->adapter_properties_cb(BT_STATUS_SUCCESS, p.Count(), p.Get());
}

void ReportBonded() {
  g->conn->Request(ZTBT_OP_BONDED_LIST, {}, ZTBT_SIG_BONDED_LIST_REPLY, [](uint32_t st, const Values& v) {
    if (st != ZTBT_ST_OK || !g->cb) return;
    std::vector<RawAddress> addrs;
    for (auto& t : v[0].items) {
      Dev d = DevFrom(t);
      Remember(d);
      Props p;
      DeviceProps(d, &p);
      g->cb->remote_device_properties_cb(BT_STATUS_SUCCESS, &d.addr, p.Count(), p.Get());
      addrs.push_back(d.addr);
    }
    Props p;
    p.AddRaw(BT_PROPERTY_ADAPTER_BONDED_DEVICES, addrs.data(), addrs.size() * sizeof(RawAddress));
    g->cb->adapter_properties_cb(BT_STATUS_SUCCESS, p.Count(), p.Get());
  });
}

void CloseLinks(bool tell) {
  auto links = std::move(g->links);
  g->links.clear();
  if (!tell || !g->gcb || !g->gcb->client) return;
  for (auto& [id, l] : links) {
    if (l.open) g->gcb->client->close_cb((int)id, 0x16, l.client_if, l.addr);
    else g->gcb->client->open_cb((int)id, kGattError, l.client_if, l.addr);
  }
}

void BridgeEnable() {
  if (!g->want_on || !g->conn->connected() || !g->hello) return;
  g->conn->Request(ZTBT_OP_ENABLE, {Value::Bool(true)}, ZTBT_SIG_ENABLE_REPLY, [](uint32_t st, const Values&) {
    if (!g->want_on) return;
    if (st != ZTBT_ST_OK) {
      LOGW("enable: bridge said %u; again in 2 s", st);
      g->conn->After(2000, BridgeEnable);
      return;
    }
    g->bridge_on = true;
    if (!g->on) {
      g->on = true;
      g->cb->adapter_state_changed_cb(BT_STATE_ON);
    }
    ReportAdapter();
    ReportBonded();
    if (g->le_scan || g->discovery) {   // a reconnect: carry on scanning
      g->conn->Request(ZTBT_OP_SCAN_START, {Value::Y(g->discovery ? T_AUTO : T_LE)}, "",
                       [](uint32_t, const Values&) {});
    }
  });
}

void Connected() {
  g->hello = false;
  g->bridge_on = false;
  g->conn->Request(ZTBT_OP_HELLO, {Value::U32(kVersion), Value::Str("waydroid-bluetooth")},
                   ZTBT_SIG_HELLO_REPLY, [](uint32_t st, const Values& v) {
    if (st != ZTBT_ST_OK) {
      LOGE("bridge refused HELLO (%u)", st);
      return;
    }
    g->hello = true;
    v[1].addr(g->address.address);
    g->name = v[2].s;
    g->cls = v[3].num();
    LOGI("bridge: adapter %s \"%s\" powered %d", AddrText(g->address.address).c_str(), g->name.c_str(),
         (int)v[4].flag());
    BridgeEnable();
  });
}

void Disconnected() {
  g->hello = false;
  g->bridge_on = false;
  CloseLinks(true);
  {
    std::lock_guard<std::mutex> l(g->cache_mu);
    g->acl.clear();
  }
  // Android stays "on": the bridge comes back (it restarts with bluetoothd)
}

// ---- events ------------------------------------------------------------------

void OnAdapter(const Values& v) {
  g->name = v[1].s;
  ReportAdapter(BT_PROPERTY_BDNAME);
}

void OnFound(const Values& v) {
  Dev d = DevFrom(v[0]);
  Remember(d);
  int rssi = v[1].sint();
  int tx = v[2].sint();
  if (g->le_scan && g->scan_cb) {
    std::vector<uint8_t> ad(v[3].s.begin(), v[3].s.end());
    // legacy, connectable, scannable: BlueZ doesn't say which it was
    g->scan_cb->OnScanResult(0x13, d.addr_type, d.addr, 1, 0, 0xff, (int8_t)tx, (int8_t)rssi, 0, ad);
  }
  if (g->discovery && g->cb && g->found.insert(Key(d.addr)).second) {
    Props p;
    DeviceProps(d, &p, true, (int8_t)rssi);
    g->cb->device_found_cb(p.Count(), p.Get());
  }
}

void OnBond(const Values& v) {
  RawAddress a = AddrFrom(v[0]);
  bt_bond_state_t state = (bt_bond_state_t)v[1].num();
  bt_status_t st = BtStatus(v[2].num());
  if (state == BT_BOND_STATE_BONDED) {
    // the device's properties first, as Android's own stack does
    g->conn->Request(ZTBT_OP_DEVICE_GET, {Value::Addr(a.address)}, ZTBT_SIG_DEVICE_GET_REPLY,
                     [a, state, st](uint32_t rst, const Values& dv) {
      RawAddress addr = a;
      if (rst == ZTBT_ST_OK) {
        Dev d = DevFrom(dv[0]);
        Remember(d);
        Props p;
        DeviceProps(d, &p);
        g->cb->remote_device_properties_cb(BT_STATUS_SUCCESS, &addr, p.Count(), p.Get());
      }
      g->cb->bond_state_changed_cb(st, &addr, state, 0);
    });
    return;
  }
  g->cb->bond_state_changed_cb(st, &a, state, 0);
}

void OnAcl(const Values& v) {
  RawAddress a = AddrFrom(v[0]);
  bool up = v[1].flag();
  {
    std::lock_guard<std::mutex> l(g->cache_mu);
    if (up) g->acl.insert(Key(a));
    else g->acl.erase(Key(a));
  }
  g->cb->acl_state_changed_cb(BT_STATUS_SUCCESS, &a, up ? BT_ACL_STATE_CONNECTED : BT_ACL_STATE_DISCONNECTED,
                              (int)v[2].num(), up ? 0 : 0x13);
}

void OnScanStopped(const Values& v) {
  if (v[0].num() != T_LE && g->discovery) {
    g->discovery = false;
    g->cb->discovery_state_changed_cb(BT_DISCOVERY_STOPPED);
  }
}

void OnLink(const Values& v) {
  uint32_t id = v[0].num();
  auto it = g->links.find(id);
  if (it == g->links.end() || !g->gcb) return;
  Link& l = it->second;
  bool up = v[2].flag();
  if (up && !l.open) {
    l.open = true;
    g->gcb->client->open_cb((int)id, kGattSuccess, l.client_if, l.addr);
    return;
  }
  if (!up) {
    Link gone = l;
    g->links.erase(it);
    if (gone.open) g->gcb->client->close_cb((int)id, v[1].num() == ZTBT_ST_OK ? 0x13 : 0x08, gone.client_if, gone.addr);
    else g->gcb->client->open_cb((int)id, kGattError, gone.client_if, gone.addr);
  }
}

void OnValue(const Values& v) {
  uint32_t id = v[0].num();
  auto it = g->links.find(id);
  if (it == g->links.end() || !g->gcb) return;
  btgatt_notify_params_t p;
  memset(&p, 0, sizeof p);
  size_t n = std::min(v[2].s.size(), sizeof p.value);
  memcpy(p.value, v[2].s.data(), n);
  p.len = (uint16_t)n;
  p.bda = it->second.addr;
  p.handle = (uint16_t)v[1].num();
  p.is_notify = 1;
  g->gcb->client->notify_cb((int)id, p);
}

// ---- bt_interface_t ----------------------------------------------------------

int Init(bt_callbacks_t* callbacks, bool, bool, int, const char**, bool, const char*) {
  if (g) return BT_STATUS_DONE;
  g = new State;
  g->cb = callbacks;
  const char* path = getenv("ZTBT_SOCKET");   // host tests only
  g->conn = new Conn(path ? path : kSocketPath);
  Conn* c = g->conn;
  c->OnConnected(Connected);
  c->OnDisconnected(Disconnected);
  c->OnEvent(ZTBT_OP_EV_ADAPTER, ZTBT_SIG_EV_ADAPTER, OnAdapter);
  c->OnEvent(ZTBT_OP_EV_DEVICE_FOUND, ZTBT_SIG_EV_DEVICE_FOUND, OnFound);
  c->OnEvent(ZTBT_OP_EV_BOND_STATE, ZTBT_SIG_EV_BOND_STATE, OnBond);
  c->OnEvent(ZTBT_OP_EV_ACL, ZTBT_SIG_EV_ACL, OnAcl);
  c->OnEvent(ZTBT_OP_EV_SCAN_STOPPED, ZTBT_SIG_EV_SCAN_STOPPED, OnScanStopped);
  c->OnEvent(ZTBT_OP_EV_LINK, ZTBT_SIG_EV_LINK, OnLink);
  c->OnEvent(ZTBT_OP_EV_GATT_VALUE, ZTBT_SIG_EV_GATT_VALUE, OnValue);
  c->Start([] { if (g->cb->thread_evt_cb) g->cb->thread_evt_cb(ASSOCIATE_JVM); },
           [] { if (g->cb->thread_evt_cb) g->cb->thread_evt_cb(DISASSOCIATE_JVM); });
  LOGI("ztOS Bluetooth shim: Android's stack through ztos-btbridge (BlueZ)");
  return BT_STATUS_SUCCESS;
}

int Enable() {
  if (!g) return BT_STATUS_NOT_READY;
  g->conn->Post([] {
    g->want_on = true;
    BridgeEnable();
  });
  return BT_STATUS_SUCCESS;
}

int Disable() {
  if (!g) return BT_STATUS_NOT_READY;
  g->conn->Post([] {
    g->want_on = false;
    g->le_scan = false;
    if (g->discovery) {
      g->discovery = false;
      g->cb->discovery_state_changed_cb(BT_DISCOVERY_STOPPED);
    }
    CloseLinks(true);
    g->conn->Request(ZTBT_OP_ENABLE, {Value::Bool(false)}, "", [](uint32_t, const Values&) {});
    g->bridge_on = false;
    if (g->on) {
      g->on = false;
      g->cb->adapter_state_changed_cb(BT_STATE_OFF);
    }
  });
  return BT_STATUS_SUCCESS;
}

void Cleanup() {
  if (!g) return;
  g->conn->Stop();
  delete g->conn;
  delete g;
  g = nullptr;
}

int GetAdapterProperties() {
  if (!g) return BT_STATUS_NOT_READY;
  g->conn->Post([] {
    ReportAdapter();
    if (g->bridge_on) ReportBonded();
  });
  return BT_STATUS_SUCCESS;
}

int GetAdapterProperty(bt_property_type_t type) {
  if (!g) return BT_STATUS_NOT_READY;
  g->conn->Post([type] {
    if (type == BT_PROPERTY_ADAPTER_BONDED_DEVICES) ReportBonded();
    else ReportAdapter(type);
  });
  return BT_STATUS_SUCCESS;
}

int SetAdapterProperty(const bt_property_t* property) {
  if (!g || !property) return BT_STATUS_PARM_INVALID;
  bt_property_type_t type = property->type;
  switch (type) {
    case BT_PROPERTY_BDNAME:                      // the name is Linux's
    case BT_PROPERTY_ADAPTER_SCAN_MODE:           // so is being discoverable
    case BT_PROPERTY_ADAPTER_DISCOVERABLE_TIMEOUT:
    case BT_PROPERTY_LOCAL_IO_CAPS:
    case BT_PROPERTY_LOCAL_IO_CAPS_BLE:
      g->conn->Post([type] { ReportAdapter(type); });   // the value that holds
      return BT_STATUS_SUCCESS;
    default:
      return BT_STATUS_UNSUPPORTED;
  }
}

void GetDevice(const RawAddress a, std::function<void(bt_status_t, const Dev*)> fn) {
  g->conn->Request(ZTBT_OP_DEVICE_GET, {Value::Addr(a.address)}, ZTBT_SIG_DEVICE_GET_REPLY,
                   [fn](uint32_t st, const Values& v) {
    if (st != ZTBT_ST_OK) {
      fn(BtStatus(st), nullptr);
      return;
    }
    Dev d = DevFrom(v[0]);
    Remember(d);
    fn(BT_STATUS_SUCCESS, &d);
  });
}

int GetRemoteDeviceProperties(RawAddress* addr) {
  if (!g || !addr) return BT_STATUS_PARM_INVALID;
  GetDevice(*addr, [](bt_status_t st, const Dev* d) {
    if (!d) return;
    Props p;
    DeviceProps(*d, &p);
    RawAddress a = d->addr;
    g->cb->remote_device_properties_cb(st, &a, p.Count(), p.Get());
  });
  return BT_STATUS_SUCCESS;
}

int GetRemoteDeviceProperty(RawAddress* addr, bt_property_type_t) { return GetRemoteDeviceProperties(addr); }

int SetRemoteDeviceProperty(RawAddress*, const bt_property_t*) {
  return BT_STATUS_SUCCESS;   // aliases are BlueZ's; nothing Android keeps
}

int GetRemoteServiceRecord(const RawAddress&, const Uuid&) { return BT_STATUS_UNSUPPORTED; }

int GetRemoteServices(RawAddress* addr, int) { return GetRemoteDeviceProperties(addr); }

void StopDiscovery() {
  if (!g->discovery) return;
  g->discovery = false;
  if (g->discovery_timer) g->conn->Cancel(g->discovery_timer);
  g->discovery_timer = 0;
  g->conn->Request(ZTBT_OP_SCAN_STOP, {Value::Y(T_AUTO)}, "", [](uint32_t, const Values&) {});
  g->cb->discovery_state_changed_cb(BT_DISCOVERY_STOPPED);
}

int StartDiscovery() {
  if (!g) return BT_STATUS_NOT_READY;
  g->conn->Post([] {
    if (g->discovery) return;
    g->conn->Request(ZTBT_OP_SCAN_START, {Value::Y(T_AUTO)}, "", [](uint32_t st, const Values&) {
      if (st != ZTBT_ST_OK) {
        LOGW("discovery refused (%u)", st);
        g->cb->discovery_state_changed_cb(BT_DISCOVERY_STOPPED);
        return;
      }
      g->discovery = true;
      g->found.clear();
      g->cb->discovery_state_changed_cb(BT_DISCOVERY_STARTED);
      g->discovery_timer = g->conn->After(kDiscoveryMs, [] {
        g->discovery_timer = 0;
        StopDiscovery();
      });
    });
  });
  return BT_STATUS_SUCCESS;
}

int CancelDiscovery() {
  if (!g) return BT_STATUS_NOT_READY;
  g->conn->Post(StopDiscovery);
  return BT_STATUS_SUCCESS;
}

int CreateBond(const RawAddress* addr, int transport) {
  if (!g || !addr) return BT_STATUS_PARM_INVALID;
  RawAddress a = *addr;
  uint8_t t = transport == 1 ? T_BREDR : transport == 2 ? T_LE : T_AUTO;
  g->conn->Request(ZTBT_OP_BOND_CREATE, {Value::Addr(a.address), Value::Y(t)}, "", [a](uint32_t st, const Values&) {
    if (st != ZTBT_ST_OK) {
      RawAddress addr2 = a;
      g->cb->bond_state_changed_cb(BtStatus(st), &addr2, BT_BOND_STATE_NONE, 0);
    }
    // otherwise BONDING and then BONDED/NONE come as events
  });
  return BT_STATUS_SUCCESS;
}

int CreateBondOob(const RawAddress*, int, const bt_oob_data_t*, const bt_oob_data_t*) {
  return BT_STATUS_UNSUPPORTED;
}

int RemoveBond(const RawAddress* addr) {
  if (!g || !addr) return BT_STATUS_PARM_INVALID;
  uint32_t st;
  // Android's removeBond() returns this: wait for the bridge's answer
  if (!g->conn->Call(ZTBT_OP_BOND_REMOVE, {Value::Addr(addr->address)}, "", &st, nullptr)) {
    return BT_STATUS_FAIL;
  }
  return st == ZTBT_ST_OK ? BT_STATUS_SUCCESS : BT_STATUS_FAIL;
}

int CancelBond(const RawAddress* addr) {
  if (!g || !addr) return BT_STATUS_PARM_INVALID;
  g->conn->Request(ZTBT_OP_BOND_CANCEL, {Value::Addr(addr->address)}, "", [](uint32_t, const Values&) {});
  return BT_STATUS_SUCCESS;
}

int GetConnectionState(const RawAddress* addr) {
  if (!g || !addr) return 0;
  std::lock_guard<std::mutex> l(g->cache_mu);
  return g->acl.count(Key(*addr)) ? 1 : 0;
}

int PinReply(const RawAddress*, uint8_t, uint8_t, bt_pin_code_t*) { return BT_STATUS_UNSUPPORTED; }
int SspReply(const RawAddress*, bt_ssp_variant_t, uint8_t, uint32_t) { return BT_STATUS_UNSUPPORTED; }

const void* GetProfileInterface(const char* id);

int DutModeConfigure(uint8_t) { return BT_STATUS_UNSUPPORTED; }            // no HCI for Android
int DutModeSend(uint16_t, uint8_t*, uint8_t) { return BT_STATUS_UNSUPPORTED; }
int LeTestMode(uint16_t, uint8_t*, uint8_t) { return BT_STATUS_UNSUPPORTED; }
int SetOsCallouts(bt_os_callouts_t*) { return BT_STATUS_SUCCESS; }
int ReadEnergyInfo() { return BT_STATUS_UNSUPPORTED; }

void Dump(int fd, const char**) {
  if (!g) return;
  std::string s = "ztOS Bluetooth shim (BlueZ through ztos-btbridge)\n  bridge: ";
  s += g->conn->connected() ? "connected" : "not connected";
  s += "\n  enabled: " + std::string(g->bridge_on ? "yes" : "no") + "\n";
  (void)!write(fd, s.data(), s.size());
}

void DumpMetrics(std::string* out) { if (out) out->clear(); }
int ConfigClear() { return BT_STATUS_SUCCESS; }
void InteropClear() {}
void InteropAdd(uint16_t, const RawAddress*, size_t) {}
bluetooth::avrcp::ServiceInterface* GetAvrcpService() { return nullptr; }
std::string ObfuscateAddress(const RawAddress&) { return std::string(); }
int GetMetricId(const RawAddress&) { return 0; }
int SetDynamicAudioBufferSize(int, int) { return BT_STATUS_UNSUPPORTED; }
int GenerateLocalOobData(tBT_TRANSPORT) { return BT_STATUS_UNSUPPORTED; }
bool AllowLowLatencyAudio(bool, const RawAddress&) { return false; }
int ClearEventFilter() { return BT_STATUS_SUCCESS; }
void MetadataChanged(const RawAddress&, int, std::vector<uint8_t>) {}

// ---- GATT client -------------------------------------------------------------

Link* FindLink(int conn_id) {
  auto it = g->links.find((uint32_t)conn_id);
  return it == g->links.end() ? nullptr : &it->second;
}

int LinkOf(int client_if, const RawAddress& a) {
  for (auto& [id, l] : g->links)
    if (l.client_if == client_if && l.addr == a) return (int)id;
  return 0;
}

bt_status_t RegisterClient(const Uuid& uuid, bool) {
  if (!g) return BT_STATUS_NOT_READY;
  Uuid u = uuid;
  g->conn->Post([u] {
    int id = g->next_client++;
    g->clients[id] = u;
    g->gcb->client->register_client_cb(kGattSuccess, id, u);
  });
  return BT_STATUS_SUCCESS;
}

bt_status_t UnregisterClient(int client_if) {
  if (!g) return BT_STATUS_NOT_READY;
  g->conn->Post([client_if] {
    g->clients.erase(client_if);
    for (auto it = g->links.begin(); it != g->links.end();) {
      if (it->second.client_if == client_if) {
        g->conn->Request(ZTBT_OP_GATT_CLOSE, {Value::U32(it->first)}, "", [](uint32_t, const Values&) {});
        it = g->links.erase(it);
      } else {
        ++it;
      }
    }
  });
  return BT_STATUS_SUCCESS;
}

bt_status_t ClientConnect(int client_if, const RawAddress& bd_addr, bool is_direct, int transport, bool, int) {
  if (!g) return BT_STATUS_NOT_READY;
  RawAddress a = bd_addr;
  // background (autoConnect) connections are made directly too: BlueZ owns the
  // controller's accept list
  (void)is_direct;
  (void)transport;
  g->conn->Post([client_if, a] {
    if (LinkOf(client_if, a)) return;   // already open or opening
    g->conn->Request(ZTBT_OP_GATT_OPEN, {Value::Addr(a.address), Value::Y(0), Value::Y(T_LE)},
                     ZTBT_SIG_GATT_OPEN_REPLY, [client_if, a](uint32_t st, const Values& v) {
      RawAddress addr = a;
      if (st != ZTBT_ST_OK) {
        LOGW("connect %s: bridge said %u", AddrText(a.address).c_str(), st);
        g->gcb->client->open_cb(0, kGattError, client_if, addr);
        return;
      }
      Link l;
      l.client_if = client_if;
      l.addr = a;
      g->links[v[0].num()] = l;
    });
  });
  return BT_STATUS_SUCCESS;
}

bt_status_t ClientDisconnect(int client_if, const RawAddress& bd_addr, int conn_id) {
  if (!g) return BT_STATUS_NOT_READY;
  RawAddress a = bd_addr;
  g->conn->Post([client_if, a, conn_id] {
    int id = conn_id && FindLink(conn_id) ? conn_id : LinkOf(client_if, a);
    RawAddress addr = a;
    if (!id) {
      g->gcb->client->close_cb(conn_id, kGattSuccess, client_if, addr);
      return;
    }
    g->links.erase((uint32_t)id);
    g->conn->Request(ZTBT_OP_GATT_CLOSE, {Value::U32((uint32_t)id)}, "", [](uint32_t, const Values&) {});
    g->gcb->client->close_cb(id, kGattSuccess, client_if, addr);
  });
  return BT_STATUS_SUCCESS;
}

bt_status_t Refresh(int, const RawAddress&) { return BT_STATUS_SUCCESS; }

bt_status_t SearchService(int conn_id, const Uuid*) {
  if (!g) return BT_STATUS_NOT_READY;
  g->conn->Post([conn_id] {
    if (!FindLink(conn_id)) {
      g->gcb->client->search_complete_cb(conn_id, kGattError);
      return;
    }
    g->conn->Request(ZTBT_OP_GATT_DISCOVER, {Value::U32((uint32_t)conn_id)}, ZTBT_SIG_GATT_DISCOVER_REPLY,
                     [conn_id](uint32_t st, const Values& v) {
      Link* l = FindLink(conn_id);
      if (st != ZTBT_ST_OK || !l) {
        g->gcb->client->search_complete_cb(conn_id, GattStatus(st, v));
        return;
      }
      l->db.clear();
      uint16_t n = 0;
      for (auto& e : v[0].items) {
        btgatt_db_element_t el;
        memset(&el, 0, sizeof el);
        el.id = n++;
        el.type = (bt_gatt_db_attribute_type_t)e.items[0].num();
        el.attribute_handle = (uint16_t)e.items[1].num();
        el.start_handle = (uint16_t)e.items[2].num();
        el.end_handle = (uint16_t)e.items[3].num();
        el.uuid = UuidFrom(e.items[4].uuid());
        el.properties = (uint8_t)e.items[5].num();
        l->db.push_back(el);
      }
      g->gcb->client->search_complete_cb(conn_id, kGattSuccess);
    });
  });
  return BT_STATUS_SUCCESS;
}

void DiscoverByUuid(int conn_id, const Uuid& uuid) { SearchService(conn_id, &uuid); }

bt_status_t GetGattDb(int conn_id) {
  if (!g) return BT_STATUS_NOT_READY;
  g->conn->Post([conn_id] {
    Link* l = FindLink(conn_id);
    if (!l) {
      g->gcb->client->get_gatt_db_cb(conn_id, nullptr, 0);
      return;
    }
    g->gcb->client->get_gatt_db_cb(conn_id, l->db.data(), (int)l->db.size());
  });
  return BT_STATUS_SUCCESS;
}

void Read(int conn_id, uint16_t handle, bool descriptor) {
  g->conn->Request(ZTBT_OP_GATT_READ, {Value::U32((uint32_t)conn_id), Value::Q(handle), Value::Q(0)},
                   ZTBT_SIG_GATT_READ_REPLY, [conn_id, handle, descriptor](uint32_t st, const Values& v) {
    btgatt_read_params_t p;
    memset(&p, 0, sizeof p);
    p.handle = handle;
    int status = GattStatus(st, v);
    if (st == ZTBT_ST_OK) {
      size_t n = std::min(v[0].s.size(), sizeof p.value.value);
      memcpy(p.value.value, v[0].s.data(), n);
      p.value.len = (uint16_t)n;
    }
    p.status = (uint8_t)status;
    if (descriptor) g->gcb->client->read_descriptor_cb(conn_id, status, p);
    else g->gcb->client->read_characteristic_cb(conn_id, status, &p);
  });
}

bt_status_t ReadCharacteristic(int conn_id, uint16_t handle, int) {
  if (!g) return BT_STATUS_NOT_READY;
  Read(conn_id, handle, false);
  return BT_STATUS_SUCCESS;
}

bt_status_t ReadDescriptor(int conn_id, uint16_t handle, int) {
  if (!g) return BT_STATUS_NOT_READY;
  Read(conn_id, handle, true);
  return BT_STATUS_SUCCESS;
}

bt_status_t ReadByUuid(int conn_id, const Uuid&, uint16_t, uint16_t, int) {
  if (!g) return BT_STATUS_NOT_READY;
  g->conn->Post([conn_id] {
    btgatt_read_params_t p;
    memset(&p, 0, sizeof p);
    p.status = kGattReqNotSupported;
    g->gcb->client->read_characteristic_cb(conn_id, kGattReqNotSupported, &p);
  });
  return BT_STATUS_SUCCESS;
}

void Write(int conn_id, uint16_t handle, int wtype, const uint8_t* value, size_t len, bool descriptor) {
  std::string val((const char*)value, len);
  g->conn->Request(ZTBT_OP_GATT_WRITE,
                   {Value::U32((uint32_t)conn_id), Value::Q(handle), Value::Y((uint8_t)wtype), Value::Q(0),
                    Value::Bytes(val)},
                   "", [conn_id, handle, val, descriptor](uint32_t st, const Values& v) {
    int status = GattStatus(st, v);
    if (descriptor)
      g->gcb->client->write_descriptor_cb(conn_id, status, handle, (uint16_t)val.size(), (const uint8_t*)val.data());
    else
      g->gcb->client->write_characteristic_cb(conn_id, status, handle, (uint16_t)val.size(),
                                              (const uint8_t*)val.data());
  });
}

bt_status_t WriteCharacteristic(int conn_id, uint16_t handle, int write_type, int, const uint8_t* value,
                                size_t length) {
  if (!g) return BT_STATUS_NOT_READY;
  Write(conn_id, handle, write_type, value, length, false);
  return BT_STATUS_SUCCESS;
}

bt_status_t WriteDescriptor(int conn_id, uint16_t handle, int, const uint8_t* value, size_t length) {
  if (!g) return BT_STATUS_NOT_READY;
  Write(conn_id, handle, W_DEFAULT, value, length, true);
  return BT_STATUS_SUCCESS;
}

bt_status_t ExecuteWrite(int conn_id, int execute) {
  if (!g) return BT_STATUS_NOT_READY;
  g->conn->Post([conn_id, execute] {
    g->gcb->client->execute_write_cb(conn_id, execute ? kGattReqNotSupported : kGattSuccess);
  });
  return BT_STATUS_SUCCESS;
}

// Android's own bookkeeping: the CCCD write the app makes is what the bridge
// turns into StartNotify.
bt_status_t RegisterNotify(int client_if, const RawAddress& addr, uint16_t handle, bool on) {
  if (!g) return BT_STATUS_NOT_READY;
  RawAddress a = addr;
  g->conn->Post([client_if, a, handle, on] {
    int id = LinkOf(client_if, a);
    g->gcb->client->register_for_notification_cb(id, on ? 1 : 0, id ? kGattSuccess : kGattError, handle);
  });
  return BT_STATUS_SUCCESS;
}

bt_status_t RegisterForNotification(int client_if, const RawAddress& addr, uint16_t handle) {
  return RegisterNotify(client_if, addr, handle, true);
}

bt_status_t DeregisterForNotification(int client_if, const RawAddress& addr, uint16_t handle) {
  return RegisterNotify(client_if, addr, handle, false);
}

bt_status_t ReadRemoteRssi(int client_if, const RawAddress& addr) {
  if (!g) return BT_STATUS_NOT_READY;
  RawAddress a = addr;
  g->conn->Request(ZTBT_OP_RSSI, {Value::Addr(a.address)}, ZTBT_SIG_RSSI_REPLY,
                   [client_if, a](uint32_t st, const Values& v) {
    g->gcb->client->read_remote_rssi_cb(client_if, a, st == ZTBT_ST_OK ? v[0].sint() : 0,
                                        st == ZTBT_ST_OK ? kGattSuccess : kGattError);
  });
  return BT_STATUS_SUCCESS;
}

int GetDeviceType(const RawAddress& addr) {
  if (!g) return BT_DEVICE_DEVTYPE_BLE;
  std::lock_guard<std::mutex> l(g->cache_mu);
  auto it = g->devs.find(Key(addr));
  return it == g->devs.end() ? (int)BT_DEVICE_DEVTYPE_BLE : (int)it->second.type;
}

bt_status_t ConfigureMtu(int conn_id, int) {
  if (!g) return BT_STATUS_NOT_READY;
  g->conn->Request(ZTBT_OP_GATT_MTU, {Value::U32((uint32_t)conn_id)}, ZTBT_SIG_GATT_MTU_REPLY,
                   [conn_id](uint32_t st, const Values& v) {
    // BlueZ exchanged the MTU when it connected: report what it got
    g->gcb->client->configure_mtu_cb(conn_id, st == ZTBT_ST_OK ? kGattSuccess : kGattError,
                                     st == ZTBT_ST_OK ? (int)v[0].num() : 23);
  });
  return BT_STATUS_SUCCESS;
}

bt_status_t ConnParameterUpdate(const RawAddress&, int, int, int, int, uint16_t, uint16_t) {
  return BT_STATUS_SUCCESS;   // BlueZ and the peer decide the parameters
}

bt_status_t SetPreferredPhy(const RawAddress&, uint8_t, uint8_t, uint16_t) { return BT_STATUS_SUCCESS; }

bt_status_t ReadPhy(const RawAddress&, base::Callback<void(uint8_t, uint8_t, uint8_t)> cb) {
  if (!g) return BT_STATUS_NOT_READY;
  g->conn->Post([cb] { cb.Run(1, 1, 0); });   // 1M both ways
  return BT_STATUS_SUCCESS;
}

bt_status_t TestCommand(int, const btgatt_test_params_t&) { return BT_STATUS_UNSUPPORTED; }

const btgatt_client_interface_t kClient = {
    RegisterClient,
    UnregisterClient,
    ClientConnect,
    ClientDisconnect,
    Refresh,
    SearchService,
    DiscoverByUuid,
    ReadCharacteristic,
    ReadByUuid,
    WriteCharacteristic,
    ReadDescriptor,
    WriteDescriptor,
    ExecuteWrite,
    RegisterForNotification,
    DeregisterForNotification,
    ReadRemoteRssi,
    GetDeviceType,
    ConfigureMtu,
    ConnParameterUpdate,
    SetPreferredPhy,
    ReadPhy,
    TestCommand,
    GetGattDb,
};

// ---- GATT server (stage 2: refuses) ------------------------------------------

bt_status_t RegisterServer(const Uuid& uuid, bool) {
  if (!g) return BT_STATUS_NOT_READY;
  Uuid u = uuid;
  g->conn->Post([u] { g->gcb->server->register_server_cb(kGattError, 0, u); });
  return BT_STATUS_SUCCESS;
}
bt_status_t UnregisterServer(int) { return BT_STATUS_SUCCESS; }
bt_status_t ServerConnect(int, const RawAddress&, bool, int) { return BT_STATUS_UNSUPPORTED; }
bt_status_t ServerDisconnect(int, const RawAddress&, int) { return BT_STATUS_UNSUPPORTED; }
bt_status_t AddService(int, const btgatt_db_element_t*, size_t) { return BT_STATUS_UNSUPPORTED; }
bt_status_t StopService(int, int) { return BT_STATUS_UNSUPPORTED; }
bt_status_t DeleteService(int, int) { return BT_STATUS_UNSUPPORTED; }
bt_status_t SendIndication(int, int, int, int, const uint8_t*, size_t) { return BT_STATUS_UNSUPPORTED; }
bt_status_t SendResponse(int, int, int, const btgatt_response_t&) { return BT_STATUS_UNSUPPORTED; }

const btgatt_server_interface_t kServer = {
    RegisterServer, UnregisterServer, ServerConnect, ServerDisconnect, AddService, StopService,
    DeleteService,  SendIndication,   SendResponse,  SetPreferredPhy,  ReadPhy,
};

// ---- LE scanner ----------------------------------------------------------------

class Scanner : public BleScannerInterface {
 public:
  void RegisterScanner(const Uuid&, RegisterCallback cb) override {
    g->conn->Post([this, cb] { cb.Run((uint8_t)next_++, 0); });
  }
  void Unregister(int) override {}
  void Scan(bool start) override {
    g->conn->Post([start] {
      if (g->le_scan == start) return;
      g->le_scan = start;
      g->conn->Request(start ? ZTBT_OP_SCAN_START : ZTBT_OP_SCAN_STOP, {Value::Y(T_LE)}, "",
                       [start](uint32_t st, const Values&) {
        if (st != ZTBT_ST_OK) LOGW("LE scan %s: bridge said %u", start ? "start" : "stop", st);
      });
    });
  }
  // No filters offloaded (max_adv_filter_supported is 0): Android filters itself
  void ScanFilterParamSetup(uint8_t, uint8_t action, uint8_t, std::unique_ptr<btgatt_filt_param_setup_t>,
                            FilterParamSetupCallback cb) override {
    g->conn->Post([cb, action] { cb.Run(0, action, 1); });
  }
  void ScanFilterAdd(int, std::vector<ApcfCommand>, FilterConfigCallback cb) override {
    g->conn->Post([cb] { cb.Run(0, 0, 0, 1); });
  }
  void ScanFilterClear(int, FilterConfigCallback cb) override {
    g->conn->Post([cb] { cb.Run(0, 0, 0, 1); });
  }
  void ScanFilterEnable(bool enable, EnableCallback cb) override {
    g->conn->Post([cb, enable] { cb.Run(enable, 1); });
  }
  void SetScanParameters(int, int, int, Callback cb) override {
    g->conn->Post([cb] { cb.Run(0); });   // BlueZ picks the intervals
  }
  void BatchscanConfigStorage(int, int, int, int, Callback cb) override { g->conn->Post([cb] { cb.Run(1); }); }
  void BatchscanEnable(int, int, int, int, int, Callback cb) override { g->conn->Post([cb] { cb.Run(1); }); }
  void BatchscanDisable(Callback cb) override { g->conn->Post([cb] { cb.Run(1); }); }
  void BatchscanReadReports(int, int) override {}
  void StartSync(uint8_t sid, RawAddress address, uint16_t, uint16_t, int reg_id) override {
    g->conn->Post([sid, address, reg_id] {
      if (g->scan_cb) g->scan_cb->OnPeriodicSyncStarted(reg_id, 1, 0, sid, 0, address, 0, 0);
    });
  }
  void StopSync(uint16_t) override {}
  void RegisterCallbacks(ScanningCallbacks* callbacks) override { g->scan_cb = callbacks; }
  void CancelCreateSync(uint8_t, RawAddress) override {}
  void TransferSync(RawAddress, uint16_t, uint16_t, int) override {}
  void TransferSetInfo(RawAddress, uint16_t, uint8_t, int) override {}
  void SyncTxParameters(RawAddress, uint8_t, uint16_t, uint16_t, int) override {}

 private:
  int next_ = 1;
};

// ---- LE advertiser (stage 2: refuses) ------------------------------------------

constexpr uint8_t kAdvUnsupported = 0x0c;   // "command disallowed": Android reports a failure

class Advertiser : public BleAdvertiserInterface {
 public:
  void RegisterAdvertiser(IdStatusCallback cb) override {
    g->conn->Post([cb] { cb.Run(0, kAdvUnsupported); });
  }
  void GetOwnAddress(uint8_t, GetAddressCallback cb) override {
    g->conn->Post([cb] { cb.Run(0, g->address); });
  }
  void SetParameters(uint8_t, AdvertiseParameters, ParametersCallback cb) override {
    g->conn->Post([cb] { cb.Run(kAdvUnsupported, 0); });
  }
  void SetData(int, bool, std::vector<uint8_t>, StatusCallback cb) override {
    g->conn->Post([cb] { cb.Run(kAdvUnsupported); });
  }
  void Enable(uint8_t, bool, StatusCallback cb, uint16_t, uint8_t, StatusCallback) override {
    g->conn->Post([cb] { cb.Run(kAdvUnsupported); });
  }
  void Unregister(uint8_t) override {}
  void StartAdvertising(uint8_t, StatusCallback cb, AdvertiseParameters, std::vector<uint8_t>,
                        std::vector<uint8_t>, int, StatusCallback) override {
    g->conn->Post([cb] { cb.Run(kAdvUnsupported); });
  }
  void StartAdvertisingSet(int, IdTxPowerStatusCallback register_cb, AdvertiseParameters, std::vector<uint8_t>,
                           std::vector<uint8_t>, PeriodicAdvertisingParameters, std::vector<uint8_t>, uint16_t,
                           uint8_t, IdStatusCallback) override {
    g->conn->Post([register_cb] { register_cb.Run(0, 0, kAdvUnsupported); });
  }
  void SetPeriodicAdvertisingParameters(int, PeriodicAdvertisingParameters, StatusCallback cb) override {
    g->conn->Post([cb] { cb.Run(kAdvUnsupported); });
  }
  void SetPeriodicAdvertisingData(int, std::vector<uint8_t>, StatusCallback cb) override {
    g->conn->Post([cb] { cb.Run(kAdvUnsupported); });
  }
  void SetPeriodicAdvertisingEnable(int, bool, StatusCallback cb) override {
    g->conn->Post([cb] { cb.Run(kAdvUnsupported); });
  }
  void RegisterCallbacks(AdvertisingCallbacks* callbacks) override { if (g) g->adv_cb = callbacks; }
};

Scanner gScanner;
Advertiser gAdvertiser;

bt_status_t GattInit(const btgatt_callbacks_t* callbacks) {
  if (!g) return BT_STATUS_NOT_READY;
  g->gcb = callbacks;
  return BT_STATUS_SUCCESS;
}

void GattCleanup() {
  if (g) g->gcb = nullptr;
}

const btgatt_interface_t kGatt = {
    sizeof(btgatt_interface_t), GattInit, GattCleanup, &kClient, &kServer, &gScanner, &gAdvertiser,
};

const void* GetProfileInterface(const char* id) {
  if (id && strcmp(id, BT_PROFILE_GATT_ID) == 0) return &kGatt;
  return nullptr;   // sockets: stage 3; audio, HID, PAN...: Linux's
}

}  // namespace

const bt_interface_t kInterface = {
    sizeof(bt_interface_t),
    Init,
    Enable,
    Disable,
    Cleanup,
    GetAdapterProperties,
    GetAdapterProperty,
    SetAdapterProperty,
    GetRemoteDeviceProperties,
    GetRemoteDeviceProperty,
    SetRemoteDeviceProperty,
    GetRemoteServiceRecord,
    GetRemoteServices,
    StartDiscovery,
    CancelDiscovery,
    CreateBond,
    CreateBondOob,
    RemoveBond,
    CancelBond,
    GetConnectionState,
    PinReply,
    SspReply,
    GetProfileInterface,
    DutModeConfigure,
    DutModeSend,
    LeTestMode,
    SetOsCallouts,
    ReadEnergyInfo,
    Dump,
    DumpMetrics,
    ConfigClear,
    InteropClear,
    InteropAdd,
    GetAvrcpService,
    ObfuscateAddress,
    GetMetricId,
    SetDynamicAudioBufferSize,
    GenerateLocalOobData,
    AllowLowLatencyAudio,
    ClearEventFilter,
    MetadataChanged,
};

}  // namespace ztbt
