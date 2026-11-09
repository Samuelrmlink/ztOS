// SPDX-License-Identifier: GPL-2.0-or-later
// The shim on the PC, driven the way Android's JNI drives it, against
// ztos-btbridge with a fake BlueZ (tests/test_shim_host.py starts both).
// Checks the callbacks Android would get; exits non-zero on the first miss.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <base/bind.h>
#include <base/callback.h>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <hardware/bluetooth.h>
#include <hardware/bt_gatt.h>
#include <hardware/bt_sock.h>
#include <poll.h>
#include <unistd.h>

#include "shim.h"

namespace {

std::mutex mu;
std::condition_variable cv;
std::vector<std::string> events;
std::thread::id jni_thread;
bool wrong_thread = false;

void Log(const std::string& e) {
  std::lock_guard<std::mutex> l(mu);
  if (jni_thread != std::thread::id() && std::this_thread::get_id() != jni_thread) wrong_thread = true;
  events.push_back(e);
  fprintf(stderr, "  event: %s\n", e.c_str());
  cv.notify_all();
}

std::string Wait(const std::string& prefix, int ms = 5000) {
  std::unique_lock<std::mutex> l(mu);
  std::string found;
  cv.wait_for(l, std::chrono::milliseconds(ms), [&] {
    for (auto it = events.begin(); it != events.end(); ++it) {
      if (it->compare(0, prefix.size(), prefix) == 0) {
        found = *it;
        events.erase(it);
        return true;
      }
    }
    return false;
  });
  if (found.empty()) {
    fprintf(stderr, "FAIL: no \"%s\"\n", prefix.c_str());
    exit(1);
  }
  return found;
}

void Expect(bool ok, const char* what) {
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", what);
    exit(1);
  }
}

std::string A(const RawAddress& a) {
  char b[18];
  snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", a.address[0], a.address[1], a.address[2], a.address[3],
           a.address[4], a.address[5]);
  return b;
}

bool ReadFull(int fd, uint8_t* buf, size_t n, int ms = 5000) {
  size_t got = 0;
  while (got < n) {
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, ms) <= 0) return false;
    ssize_t r = read(fd, buf + got, n - got);
    if (r <= 0) return false;
    got += (size_t)r;
  }
  return true;
}

RawAddress Addr(const char* s) {
  RawAddress a;
  unsigned v[6];
  sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
  for (int i = 0; i < 6; i++) a.address[i] = (uint8_t)v[i];
  return a;
}

// ---- bt_callbacks_t ----
void StateCb(bt_state_t s) { Log("state " + std::to_string(s)); }
void AdapterPropsCb(bt_status_t, int n, bt_property_t* p) {
  for (int i = 0; i < n; i++) {
    if (p[i].type == BT_PROPERTY_ADAPTER_BONDED_DEVICES) {
      std::string s = "bonded";
      for (int k = 0; k < p[i].len / 6; k++) s += " " + A(((RawAddress*)p[i].val)[k]);
      Log(s);
    } else if (p[i].type == BT_PROPERTY_BDNAME) {
      Log("name " + std::string((char*)p[i].val, p[i].len));
    } else if (p[i].type == BT_PROPERTY_LOCAL_LE_FEATURES) {
      auto* f = (bt_local_le_features_t*)p[i].val;
      Log("le_features filters " + std::to_string(f->max_adv_filter_supported) + " adv " +
          std::to_string(f->max_adv_instance));
    }
  }
}
void RemotePropsCb(bt_status_t, RawAddress* a, int n, bt_property_t* p) {
  std::string s = "device " + A(*a);
  for (int i = 0; i < n; i++)
    if (p[i].type == BT_PROPERTY_TYPE_OF_DEVICE) s += " type " + std::to_string(*(int*)p[i].val);
  Log(s);
}
void FoundCb(int n, bt_property_t* p) {
  for (int i = 0; i < n; i++)
    if (p[i].type == BT_PROPERTY_BDADDR) Log("found " + A(*(RawAddress*)p[i].val));
}
void DiscoveryCb(bt_discovery_state_t s) { Log("discovery " + std::to_string(s)); }
void BondCb(bt_status_t st, RawAddress* a, bt_bond_state_t s, int) {
  Log("bond " + A(*a) + " " + std::to_string(s) + " status " + std::to_string(st));
}
void AclCb(bt_status_t, RawAddress* a, bt_acl_state_t s, int t, bt_hci_error_code_t) {
  Log("acl " + A(*a) + " " + std::to_string(s));
}
void ThreadCb(bt_cb_thread_evt e) {
  if (e == ASSOCIATE_JVM) {
    std::lock_guard<std::mutex> l(mu);
    jni_thread = std::this_thread::get_id();
  }
}
void PinCb(RawAddress*, bt_bdname_t*, uint32_t, bool) { Log("PIN REQUEST"); }
void SspCb(RawAddress*, bt_bdname_t*, uint32_t, bt_ssp_variant_t, uint32_t) { Log("SSP REQUEST"); }

bt_callbacks_t callbacks;

// ---- GATT client callbacks ----
void RegCb(int status, int client_if, const bluetooth::Uuid&) {
  Log("registered " + std::to_string(client_if) + " " + std::to_string(status));
}
void OpenCb(int conn, int status, int client_if, const RawAddress& a) {
  Log("open " + std::to_string(conn) + " " + std::to_string(status) + " " + A(a));
}
void CloseCb(int conn, int status, int, const RawAddress& a) {
  Log("close " + std::to_string(conn) + " " + std::to_string(status));
}
void SearchCb(int conn, int status) { Log("searched " + std::to_string(status)); }
void DbCb(int conn, const btgatt_db_element_t* db, int n) {
  std::string s = "db " + std::to_string(n);
  for (int i = 0; i < n; i++) s += " " + std::to_string(db[i].type) + ":" + std::to_string(db[i].attribute_handle);
  Log(s);
}
void ReadCharCb(int conn, int status, btgatt_read_params_t* p) {
  char hex[8] = "";
  if (p->value.len) snprintf(hex, sizeof hex, "%02x%02x", p->value.value[0], p->value.len > 1 ? p->value.value[1] : 0);
  Log("readchar " + std::to_string(status) + " " + std::to_string(p->handle) + " " + hex);
}
void ReadDescCb(int, int status, const btgatt_read_params_t& p) { Log("readdesc " + std::to_string(status)); }
void WriteCharCb(int, int status, uint16_t h, uint16_t, const uint8_t*) {
  Log("writechar " + std::to_string(status) + " " + std::to_string(h));
}
void WriteDescCb(int, int status, uint16_t h, uint16_t, const uint8_t*) {
  Log("writedesc " + std::to_string(status) + " " + std::to_string(h));
}
void RegNotifyCb(int conn, int reg, int status, uint16_t h) {
  Log("regnotify " + std::to_string(reg) + " " + std::to_string(status));
}
void NotifyCb(int conn, const btgatt_notify_params_t& p) {
  char hex[8];
  snprintf(hex, sizeof hex, "%02x", p.value[0]);
  Log("notify " + std::to_string(p.handle) + " " + hex);
}
void MtuCb(int, int status, int mtu) { Log("mtu " + std::to_string(mtu)); }
void RssiCb(int, const RawAddress&, int rssi, int status) { Log("rssi " + std::to_string(rssi)); }

// ---- GATT server callbacks ----
int server_conn = 0, server_trans = 0;
void SrvRegCb(int status, int server_if, const bluetooth::Uuid&) {
  Log("server registered " + std::to_string(server_if) + " " + std::to_string(status));
}
void SrvConnCb(int conn, int server_if, int connected, const RawAddress& a) {
  server_conn = conn;
  Log("server conn " + A(a) + " " + std::to_string(connected));
}
void SrvAddedCb(int status, int server_if, const btgatt_db_element_t* els, size_t n) {
  std::string s = "service added " + std::to_string(status);
  for (size_t i = 0; i < n; i++) s += " " + std::to_string(els[i].attribute_handle);
  Log(s);
}
void SrvDeletedCb(int status, int, int handle) { Log("service deleted " + std::to_string(status)); }
void SrvReadCb(int conn, int trans, const RawAddress& a, int handle, int offset, bool) {
  server_trans = trans;
  Log("server read " + std::to_string(handle) + " from " + A(a));
}
void SrvWriteDescCb(int conn, int trans, const RawAddress& a, int handle, int, bool need, bool, const uint8_t* v,
                    size_t n) {
  char hex[8] = "";
  if (n >= 2) snprintf(hex, sizeof hex, "%02x%02x", v[0], v[1]);
  Log("server desc write " + std::to_string(handle) + " " + hex + (need ? " need" : ""));
}
void SrvIndSentCb(int conn, int status) { Log("indication sent " + std::to_string(status)); }
void SrvMtuCb(int conn, int mtu) { Log("server mtu " + std::to_string(mtu)); }

btgatt_client_callbacks_t client_cbs;
btgatt_server_callbacks_t server_cbs;
btgatt_callbacks_t gatt_cbs;

class ScanCbs : public ScanningCallbacks {
 public:
  void OnScannerRegistered(const bluetooth::Uuid, uint8_t, uint8_t) override {}
  void OnSetScannerParameterComplete(uint8_t, uint8_t) override {}
  void OnScanResult(uint16_t, uint8_t addr_type, RawAddress bda, uint8_t, uint8_t, uint8_t, int8_t, int8_t rssi,
                    uint16_t, std::vector<uint8_t> adv) override {
    Log("scan " + A(bda) + " type " + std::to_string(addr_type) + " rssi " + std::to_string(rssi) + " ad " +
        std::to_string(adv.size()));
  }
  void OnTrackAdvFoundLost(AdvertisingTrackInfo) override {}
  void OnBatchScanReports(int, int, int, int, std::vector<uint8_t>) override {}
  void OnBatchScanThresholdCrossed(int) override {}
  void OnPeriodicSyncStarted(int, uint8_t, uint16_t, uint8_t, uint8_t, RawAddress, uint8_t, uint16_t) override {}
  void OnPeriodicSyncReport(uint16_t, int8_t, int8_t, uint8_t, std::vector<uint8_t>) override {}
  void OnPeriodicSyncLost(uint16_t) override {}
  void OnPeriodicSyncTransferred(int, uint8_t, RawAddress) override {}
};
ScanCbs scan_cbs;

void ScannerRegistered(uint8_t id, uint8_t status) { Log("scanner " + std::to_string(id) + " " + std::to_string(status)); }
void AdvRegistered(uint8_t id, int8_t tx, uint8_t status) {
  Log("advertiser " + std::to_string(id) + " tx " + std::to_string(tx) + " status " + std::to_string(status));
}
void AdvStatusCb(uint8_t status) { Log("adv status " + std::to_string(status)); }
void AdvTimeout(uint8_t id, uint8_t status) { Log("adv timeout " + std::to_string(id)); }
void PhyRead(uint8_t tx, uint8_t rx, uint8_t status) { Log("phy " + std::to_string(tx)); }

}  // namespace

int main() {
  const char* LOCK = "D0:00:00:00:00:03";
  const char* SPEAKER = "E0:00:00:00:00:04";
  const char* AIRPODS = "AA:00:00:00:00:01";
  const char* SENSOR = "C0:00:00:00:00:02";
  const bt_interface_t* bt = &ztbt::kInterface;
  Expect(bt->size == sizeof(bt_interface_t), "size");

  callbacks.size = sizeof callbacks;
  callbacks.adapter_state_changed_cb = StateCb;
  callbacks.adapter_properties_cb = AdapterPropsCb;
  callbacks.remote_device_properties_cb = RemotePropsCb;
  callbacks.device_found_cb = FoundCb;
  callbacks.discovery_state_changed_cb = DiscoveryCb;
  callbacks.pin_request_cb = PinCb;
  callbacks.ssp_request_cb = SspCb;
  callbacks.bond_state_changed_cb = BondCb;
  callbacks.acl_state_changed_cb = AclCb;
  callbacks.thread_evt_cb = ThreadCb;
  Expect(bt->init(&callbacks, false, false, 0, nullptr, false, nullptr) == BT_STATUS_SUCCESS, "init");
  Expect(bt->dut_mode_send(0x0c03, nullptr, 0) == BT_STATUS_UNSUPPORTED, "no HCI");

  // ---- enable: state, adapter, bonded devices without the Linux-connected AirPods
  Expect(bt->enable() == BT_STATUS_SUCCESS, "enable");
  Wait("state 1");
  Wait("name ztos");
  Wait("le_features filters 0 adv 4");   // BlueZ's 5, less one left to Linux
  std::string bonded = Wait("bonded");
  Expect(bonded.find(LOCK) != std::string::npos && bonded.find(SPEAKER) != std::string::npos, "bonded list");
  Expect(bonded.find(AIRPODS) == std::string::npos, "AirPods hidden");
  Wait(std::string("device ") + SPEAKER + " type 1");

  // ---- GATT and the LE scanner
  auto* gatt = (const btgatt_interface_t*)bt->get_profile_interface(BT_PROFILE_GATT_ID);
  Expect(gatt && gatt->size == sizeof(btgatt_interface_t), "gatt interface");
  Expect(bt->get_profile_interface("a2dp") == nullptr, "no a2dp");
  client_cbs.register_client_cb = RegCb;
  client_cbs.open_cb = OpenCb;
  client_cbs.close_cb = CloseCb;
  client_cbs.search_complete_cb = SearchCb;
  client_cbs.get_gatt_db_cb = DbCb;
  client_cbs.read_characteristic_cb = ReadCharCb;
  client_cbs.read_descriptor_cb = ReadDescCb;
  client_cbs.write_characteristic_cb = WriteCharCb;
  client_cbs.write_descriptor_cb = WriteDescCb;
  client_cbs.register_for_notification_cb = RegNotifyCb;
  client_cbs.notify_cb = NotifyCb;
  client_cbs.configure_mtu_cb = MtuCb;
  client_cbs.read_remote_rssi_cb = RssiCb;
  server_cbs.register_server_cb = SrvRegCb;
  server_cbs.connection_cb = SrvConnCb;
  server_cbs.service_added_cb = SrvAddedCb;
  server_cbs.service_deleted_cb = SrvDeletedCb;
  server_cbs.request_read_characteristic_cb = SrvReadCb;
  server_cbs.request_write_descriptor_cb = SrvWriteDescCb;
  server_cbs.indication_sent_cb = SrvIndSentCb;
  server_cbs.mtu_changed_cb = SrvMtuCb;
  gatt_cbs.size = sizeof gatt_cbs;
  gatt_cbs.client = &client_cbs;
  gatt_cbs.server = &server_cbs;
  Expect(gatt->init(&gatt_cbs) == BT_STATUS_SUCCESS, "gatt init");
  gatt->scanner->RegisterCallbacks(&scan_cbs);
  gatt->scanner->RegisterScanner(bluetooth::Uuid::From128BitBE(bluetooth::Uuid::UUID128Bit{}), base::Bind(&ScannerRegistered));
  Wait("scanner 1 0");
  gatt->scanner->Scan(true);
  Wait(std::string("scan ") + SENSOR + " type 1");    // the harness advertises the sensor
  gatt->scanner->Scan(false);
  // an advertising set: connectable, a name, the heart-rate service; BlueZ gets it
  AdvertiseParameters ap{};
  ap.advertising_event_properties = 0x13;   // legacy, connectable, scannable
  ap.min_interval = ap.max_interval = 160;
  ap.tx_power = -7;
  std::vector<uint8_t> ad = {0x02, 0x01, 0x06, 0x03, 0x03, 0x0d, 0x18, 0x08, 0x09, 'z', 't', '-', 't', 'e', 's', 't'};
  gatt->advertiser->StartAdvertisingSet(1, base::Bind(&AdvRegistered), ap, ad, {}, {}, {}, 0, 0,
                                        base::Bind(&AdvTimeout));
  std::string adv = Wait("advertiser ");
  Expect(adv == "advertiser 1 tx -7 status 0", "advertising set started");
  gatt->advertiser->Enable(1, false, base::Bind(&AdvStatusCb), 0, 0, BleAdvertiserInterface::StatusCallback());
  Wait("adv status 0");
  gatt->advertiser->Enable(1, true, base::Bind(&AdvStatusCb), 0, 0, BleAdvertiserInterface::StatusCallback());
  Wait("adv status 0");
  gatt->advertiser->Unregister(1);
  // periodic advertising: not through BlueZ's D-Bus API
  PeriodicAdvertisingParameters pp{};
  pp.enable = 1;
  gatt->advertiser->StartAdvertisingSet(2, base::Bind(&AdvRegistered), ap, ad, {}, pp, {}, 0, 0,
                                        base::Bind(&AdvTimeout));
  Wait("advertiser 0 tx 0 status 5");

  gatt->client->register_client(bluetooth::Uuid::From128BitBE(bluetooth::Uuid::UUID128Bit{}), false);
  std::string reg = Wait("registered ");
  int client_if = atoi(reg.c_str() + 11);
  RawAddress sensor = Addr(SENSOR);
  gatt->client->connect(client_if, sensor, true, 2, false, 1);
  std::string open = Wait("open ");
  int conn = atoi(open.c_str() + 5);
  Expect(open.find(" 0 " + std::string(SENSOR)) != std::string::npos, "open ok");
  Wait(std::string("acl ") + SENSOR + " 0");   // BT_ACL_STATE_CONNECTED
  gatt->client->search_service(conn, nullptr);
  Wait("searched 0");
  gatt->client->get_gatt_db(conn);
  Wait("db 3 0:1 3:3 4:4");
  gatt->client->read_characteristic(conn, 3, 0);
  Wait("readchar 0 3 0048");
  gatt->client->read_characteristic(conn, 99, 0);
  Wait("readchar 1 99");   // ATT invalid handle
  gatt->client->register_for_notification(client_if, sensor, 3);
  Wait("regnotify 1 0");
  uint8_t on[2] = {1, 0};
  gatt->client->write_descriptor(conn, 4, 0, on, 2);
  Wait("writedesc 0 4");
  Wait("notify 3 2a");     // the harness notifies once notifications are on
  uint8_t one = 1;
  gatt->client->write_characteristic(conn, 3, 1, 0, &one, 1);
  Wait("writechar 0 3");
  gatt->client->configure_mtu(conn, 517);
  Wait("mtu 247");
  gatt->client->read_phy(sensor, base::Bind(&PhyRead));
  Wait("phy 1");
  gatt->client->disconnect(client_if, sensor, conn);
  Wait("close ");

  // ---- the AirPods: Linux's
  RawAddress airpods = Addr(AIRPODS);
  gatt->client->connect(client_if, airpods, true, 2, false, 1);
  Wait("open 0 133 " + std::string(AIRPODS));

  // ---- bonds: pairing goes through BlueZ; removing a Linux bond is refused
  bt->create_bond(&sensor, 2);
  Wait(std::string("bond ") + SENSOR + " 1");
  Wait(std::string("bond ") + SENSOR + " 2 status 0");
  RawAddress lock = Addr(LOCK);
  Expect(bt->remove_bond(&lock) == BT_STATUS_FAIL, "remove a Linux bond refused");
  Expect(bt->remove_bond(&sensor) == BT_STATUS_SUCCESS, "remove the bond Android made");

  // ---- GATT server: a heart-rate service; the harness's central reads and subscribes
  gatt->server->register_server(bluetooth::Uuid::From128BitBE(bluetooth::Uuid::UUID128Bit{}), false);
  std::string sreg = Wait("server registered ");
  int server_if = atoi(sreg.c_str() + 18);
  auto u16 = [](uint16_t s) {
    bluetooth::Uuid::UUID128Bit b = {0, 0, (uint8_t)(s >> 8), (uint8_t)s, 0, 0, 0x10, 0, 0x80, 0, 0, 0x80, 0x5f, 0x9b, 0x34, 0xfb};
    return bluetooth::Uuid::From128BitBE(b);
  };
  btgatt_db_element_t svc[4];
  memset(svc, 0, sizeof svc);
  svc[0].type = BTGATT_DB_PRIMARY_SERVICE;
  svc[0].uuid = u16(0x180d);
  svc[1].type = BTGATT_DB_CHARACTERISTIC;
  svc[1].uuid = u16(0x2a37);
  svc[1].properties = 0x12;   // read, notify
  svc[1].permissions = 0x01;
  svc[2].type = BTGATT_DB_DESCRIPTOR;
  svc[2].uuid = u16(0x2902);
  svc[2].permissions = 0x11;
  svc[3].type = BTGATT_DB_CHARACTERISTIC;
  svc[3].uuid = u16(0x2a39);
  svc[3].properties = 0x08;
  svc[3].permissions = 0x10;
  gatt->server->add_service(server_if, svc, 4);
  std::string added = Wait("service added ");
  Expect(added == "service added 0 1 2 3 4", "service handles");
  Wait(std::string("server conn ") + SENSOR + " 1");
  Wait("server mtu 185");
  Wait("server read 2 from " + std::string(SENSOR));
  btgatt_response_t rsp;
  memset(&rsp, 0, sizeof rsp);
  rsp.attr_value.handle = 2;
  rsp.attr_value.len = 2;
  rsp.attr_value.value[0] = 0x00;
  rsp.attr_value.value[1] = 0x48;
  gatt->server->send_response(server_conn, server_trans, 0, rsp);
  Wait("server desc write 3 0100");   // the central subscribed: Android sees its CCCD written
  uint8_t hr[2] = {0x00, 0x50};
  gatt->server->send_indication(server_if, 2, server_conn, 0, hr, 2);
  Wait("indication sent 0");
  gatt->server->delete_service(server_if, 1);
  Wait("service deleted 0");
  gatt->server->unregister_server(server_if);

  // ---- Classic RFCOMM: an SPP printer (the harness's printer echoes)
  auto* sock = (const btsock_interface_t*)bt->get_profile_interface(BT_PROFILE_SOCKETS_ID);
  Expect(sock && sock->size == sizeof(btsock_interface_t), "socket interface");
  RawAddress printer = Addr("E8:00:00:00:00:05");
  bluetooth::Uuid::UUID128Bit spp_be = {0, 0, 0x11, 0x01, 0, 0, 0x10, 0, 0x80, 0, 0, 0x80, 0x5f, 0x9b, 0x34, 0xfb};
  bluetooth::Uuid spp = bluetooth::Uuid::From128BitBE(spp_be);
  int fd = -1;
  Expect(sock->connect(&printer, BTSOCK_RFCOMM, &spp, -1, &fd, 0x3, 10123) == BT_STATUS_SUCCESS && fd >= 0,
         "rfcomm connect");
  // what Android's BluetoothSocket reads first: channel, then the 20-byte signal
  uint8_t sig[24];
  Expect(ReadFull(fd, sig, sizeof sig), "channel and connect signal");
  int32_t chan, chan2, status;
  int16_t size;
  memcpy(&chan, sig, 4);
  memcpy(&size, sig + 4, 2);
  memcpy(&chan2, sig + 12, 4);
  memcpy(&status, sig + 16, 4);
  Expect(chan == 1 && size == 20 && chan2 == 1 && status == 0, "signal contents");
  Expect(memcmp(sig + 6, printer.address, 6) == 0, "signal address");
  const char job[] = "PRINT THIS\n";
  Expect(write(fd, job, sizeof job - 1) == (ssize_t)(sizeof job - 1), "write job");
  char back[sizeof job - 1];
  Expect(ReadFull(fd, (uint8_t*)back, sizeof back) && memcmp(back, job, sizeof back) == 0, "printer answered");
  close(fd);
  // the Linux-connected AirPods: refused, Android reads the end of the stream
  fd = -1;
  Expect(sock->connect(&airpods, BTSOCK_RFCOMM, &spp, -1, &fd, 0x3, 10123) == BT_STATUS_SUCCESS, "connect returns");
  Expect(!ReadFull(fd, sig, 4), "AirPods: end of stream");
  close(fd);
  Expect(sock->connect(&printer, BTSOCK_L2CAP, &spp, -1, &fd, 0, 10123) == BT_STATUS_UNSUPPORTED, "no L2CAP");

  // ---- classic discovery
  bt->start_discovery();
  Wait("discovery 1");
  bt->cancel_discovery();
  Wait("discovery 0");

  bt->disable();
  Wait("state 0");
  {
    std::lock_guard<std::mutex> l(mu);
    for (auto& e : events) Expect(e != "PIN REQUEST" && e != "SSP REQUEST", "no Android pairing dialog");
    Expect(!wrong_thread, "every callback on the associated thread");
  }
  bt->cleanup();
  printf("host test passed\n");
  return 0;
}
