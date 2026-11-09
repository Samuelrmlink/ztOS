"""ztos-btbridge's policy and its BLE central ops, against a fake BlueZ."""
import unittest

from fakes import AIRPODS, CCCD, HR, LOCK, SENSOR, SPEAKER, FakeClient, P, make
from ztbt.core import att_code, build_ad


class Basics(unittest.TestCase):
    def test_requests_before_hello_are_refused(self):
        be, store, br, c = make()
        other = FakeClient(br)
        rid = other.request("BONDED_LIST")
        self.assertEqual(other.reply(rid, "BONDED_LIST")[0], P.ST_NOT_READY)

    def test_wrong_version(self):
        be, store, br, c = make()
        other = FakeClient(br)
        rid = other.request("HELLO", 99, "x")
        self.assertEqual(other.reply(rid, "HELLO")[0], P.ST_UNSUPPORTED)

    def test_bad_packet_closes(self):
        be, store, br, c = make()
        br.handle(c, b"\x02\x00")
        self.assertTrue(c.closed)

    def test_hello_reports_adapter(self):
        be, store, br, c = make()
        st, vals = c.reply(1, "HELLO")
        self.assertEqual(vals, [P.VERSION, "00:B5:D0:3D:D8:44", "ztos", 0x6c0110, True, 5])


class Hidden(unittest.TestCase):
    """Devices Linux is connected to are not Android's."""

    def test_bonded_list_leaves_out_linux_connected(self):
        be, store, br, c = make()
        st, (devs,) = c.reply(c.request("BONDED_LIST"), "BONDED_LIST")
        self.assertEqual(st, P.ST_OK)
        self.assertEqual([d[0] for d in devs], [LOCK, SPEAKER])
        speaker = devs[1]
        self.assertEqual(speaker[4], P.DEV_BREDR)
        self.assertEqual(speaker[5], P.BOND_BONDED)
        self.assertFalse(speaker[6])

    def test_scan_never_reports_them(self):
        be, store, br, c = make()
        c.request("SCAN_START", P.T_LE)
        br.device_changed(AIRPODS, {"RSSI": -50, "ManufacturerData": {76: b"\x07\x19"}})
        br.device_changed(SENSOR, {"RSSI": -60})
        found = [e[0][0] for e in c.events("EV_DEVICE_FOUND")]
        self.assertEqual(found, [SENSOR])

    def test_connect_bond_get_rssi_busy(self):
        be, store, br, c = make()
        br.device_changed(AIRPODS, {"RSSI": -40})
        for op, args in (("GATT_OPEN", (AIRPODS, 0, P.T_LE)), ("BOND_CREATE", (AIRPODS, P.T_AUTO)),
                         ("DEVICE_GET", (AIRPODS,)), ("RSSI", (AIRPODS,))):
            self.assertEqual(c.reply(c.request(op, *args), op)[0], P.ST_BUSY, op)
        self.assertEqual(be.called("connect"), [])
        self.assertEqual(be.called("pair"), [])

    def test_shows_again_when_linux_lets_go(self):
        be, store, br, c = make()
        br.device_changed(AIRPODS, {"Connected": False})
        st, (devs,) = c.reply(c.request("BONDED_LIST"), "BONDED_LIST")
        self.assertIn(AIRPODS, [d[0] for d in devs])
        rid = c.request("GATT_OPEN", AIRPODS, 0, P.T_LE)
        self.assertEqual(c.reply(rid, "GATT_OPEN")[0], P.ST_OK)

    def test_linux_connecting_later_blocks_new_links(self):
        be, store, br, c = make()
        br.device_changed(LOCK, {"Connected": True})
        rid = c.request("GATT_OPEN", LOCK, 0, P.T_LE)
        self.assertEqual(c.reply(rid, "GATT_OPEN")[0], P.ST_BUSY)
        self.assertEqual(c.events("EV_BOND_STATE"), [])   # no fake "unpaired"

    def test_blocked_devices_hidden(self):
        be, store, br, c = make()
        br.device_changed(LOCK, {"Blocked": True})
        st, (devs,) = c.reply(c.request("BONDED_LIST"), "BONDED_LIST")
        self.assertNotIn(LOCK, [d[0] for d in devs])


class Scanning(unittest.TestCase):
    def test_needs_enable(self):
        be, store, br, c = make()
        c.request("ENABLE", False)
        rid = c.request("SCAN_START", P.T_LE)
        self.assertEqual(c.reply(rid, "SCAN_START")[0], P.ST_NOT_READY)
        self.assertEqual(be.called("discover"), [])

    def test_transports_merge_and_stop(self):
        be, store, br, c = make()
        c.request("SCAN_START", P.T_LE)
        c.request("SCAN_START", P.T_BREDR)
        c.request("SCAN_STOP", P.T_LE)
        c.request("SCAN_STOP", P.T_BREDR)
        self.assertEqual(be.called("discover"),
                         [("discover", P.T_LE), ("discover", P.T_AUTO), ("discover", P.T_BREDR), ("discover", None)])

    def test_results_are_droppable_and_carry_ad(self):
        be, store, br, c = make()
        c.request("SCAN_START", P.T_LE)
        br.device_changed(SENSOR, {"RSSI": -61, "ManufacturerData": {0x59: b"\x01\x02"},
                                   "AdvertisingFlags": b"\x06"})
        (dev, rssi, tx, ad), = c.events("EV_DEVICE_FOUND")
        self.assertEqual((dev[0], dev[1], dev[4], rssi, tx), (SENSOR, P.ADDR_RANDOM, P.DEV_BLE, -61, 127))
        self.assertEqual(ad, b"\x02\x01\x06" + b"\x03\x03\x0d\x18" + b"\x05\xff\x59\x00\x01\x02")
        self.assertEqual(c.droppable("EV_DEVICE_FOUND"), [True])

    def test_new_device_reported(self):
        be, store, br, c = make()
        c.request("SCAN_START", P.T_LE)
        br.device_changed("F0:00:00:00:00:09", {"Address": "F0:00:00:00:00:09", "AddressType": "random",
                                                 "RSSI": -70, "Connected": False}, added=True)
        self.assertEqual([e[0][0] for e in c.events("EV_DEVICE_FOUND")], ["F0:00:00:00:00:09"])

    def test_nothing_reported_when_not_scanning(self):
        be, store, br, c = make()
        br.device_changed(SENSOR, {"RSSI": -61})
        self.assertEqual(c.events("EV_DEVICE_FOUND"), [])

    def test_freeze_pauses_and_thaw_resumes(self):
        be, store, br, c = make()
        c.request("SCAN_START", P.T_LE)
        br.set_frozen(True)
        br.device_changed(SENSOR, {"RSSI": -61})
        br.set_frozen(False)
        self.assertEqual(be.called("discover"), [("discover", P.T_LE), ("discover", None), ("discover", P.T_LE)])

    def test_client_gone_stops_discovery(self):
        be, store, br, c = make()
        c.request("SCAN_START", P.T_LE)
        c.close()
        self.assertEqual(be.called("discover")[-1], ("discover", None))


class Gatt(unittest.TestCase):
    def open(self, addr=SENSOR):
        be, store, br, c = make()
        br.device_changed(addr, {"RSSI": -60})
        st, (link,) = c.reply(c.request("GATT_OPEN", addr, 1, P.T_LE), "GATT_OPEN")
        self.assertEqual(st, P.ST_OK)
        return be, store, br, c, link

    def connected(self, addr=SENSOR):
        be, store, br, c, link = self.open(addr)
        be.answer("connect")
        br.device_changed(addr, {"Connected": True})
        return be, store, br, c, link

    def test_connect_then_link_event_and_acl(self):
        be, store, br, c, link = self.connected()
        self.assertEqual(c.events("EV_LINK"), [[link, P.ST_OK, True, 247]])
        self.assertEqual(c.events("EV_ACL"), [[SENSOR, True, P.T_LE]])
        # the device Android uses is connected on Linux too, and stays Android's
        st, (dev,) = c.reply(c.request("DEVICE_GET", SENSOR), "DEVICE_GET")
        self.assertTrue(dev[6])

    def test_connect_failure(self):
        be, store, br, c, link = self.open()
        be.answer("connect", error=("org.bluez.Error.Failed", "le-connection-abort-by-local"))
        self.assertEqual(c.events("EV_LINK"), [[link, P.ST_FAIL, False, 0]])
        self.assertNotIn(SENSOR, br.holds)

    def test_connect_timeout_disconnects(self):
        be, store, br, c, link = self.open()
        (secs, fire), = be.timers
        fire()
        self.assertEqual(c.events("EV_LINK"), [[link, P.ST_TIMEOUT, False, 0]])
        self.assertEqual(be.called("disconnect"), [("disconnect", SENSOR)])

    def test_unknown_device(self):
        be, store, br, c = make()
        rid = c.request("GATT_OPEN", "F1:F1:F1:F1:F1:F1", 1, P.T_LE)
        self.assertEqual(c.reply(rid, "GATT_OPEN")[0], P.ST_NOT_FOUND)

    def test_second_link_shares_and_last_close_disconnects(self):
        be, store, br, c, link = self.connected()
        st, (link2,) = c.reply(c.request("GATT_OPEN", SENSOR, 1, P.T_LE), "GATT_OPEN")
        self.assertEqual(len(be.called("connect")), 1)
        self.assertIn([link2, P.ST_OK, True, 247], c.events("EV_LINK"))
        c.request("GATT_CLOSE", link)
        self.assertEqual(be.called("disconnect"), [])
        c.request("GATT_CLOSE", link2)
        self.assertEqual(be.called("disconnect"), [("disconnect", SENSOR)])

    def test_never_disconnects_what_it_did_not_connect(self):
        # Android's pairing made the connection ours; Linux's own connections are hidden anyway.
        # Here: the device was connected through a hold that Linux took over? No: a hold made
        # on an already connected device (only possible through our own Pair) is made=False.
        be, store, br, c = make()
        br.device_changed(LOCK, {"RSSI": -50})
        c.request("BOND_CREATE", LOCK, P.T_LE)    # already bonded: no hold
        self.assertEqual(br.holds, {})

    def test_remote_disconnect_ends_links(self):
        be, store, br, c, link = self.connected()
        br.device_changed(SENSOR, {"Connected": False})
        self.assertEqual(c.events("EV_LINK")[-1], [link, P.ST_OK, False, 0])
        self.assertEqual(be.called("disconnect"), [])
        self.assertEqual(br.links, {})

    def test_freeze_releases(self):
        be, store, br, c, link = self.connected()
        br.set_frozen(True)
        self.assertEqual(be.called("disconnect"), [("disconnect", SENSOR)])
        self.assertEqual(c.events("EV_LINK")[-1], [link, P.ST_NOT_READY, False, 0])
        rid = c.request("GATT_OPEN", SENSOR, 1, P.T_LE)
        self.assertEqual(c.reply(rid, "GATT_OPEN")[0], P.ST_NOT_READY)

    def test_client_gone_releases(self):
        be, store, br, c, link = self.connected()
        c.close()
        self.assertEqual(be.called("disconnect"), [("disconnect", SENSOR)])

    def test_discover_waits_for_services_resolved(self):
        be, store, br, c, link = self.connected()
        rid = c.request("GATT_DISCOVER", link)
        self.assertIsNone(c.reply(rid, "GATT_DISCOVER"))
        br.device_changed(SENSOR, {"ServicesResolved": True})
        st, (db,) = c.reply(rid, "GATT_DISCOVER")
        self.assertEqual(db[0], (P.DB_PRIMARY, 1, 1, 5, HR, 0))
        self.assertEqual(db[2][4], CCCD)

    def test_discover_timeout(self):
        be, store, br, c, link = self.connected()
        rid = c.request("GATT_DISCOVER", link)
        be.timers[-1][1]()
        self.assertEqual(c.reply(rid, "GATT_DISCOVER")[0], P.ST_TIMEOUT)

    def test_read_value_and_att_error(self):
        be, store, br, c, link = self.connected()
        rid = c.request("GATT_READ", link, 3, 0)
        be.answer("read", value=b"\x00\x48")
        self.assertEqual(c.reply(rid, "GATT_READ"), (P.ST_OK, [b"\x00\x48"]))
        rid = c.request("GATT_READ", link, 3, 0)
        be.answer("read", error=("org.bluez.Error.Failed", "Operation failed with ATT error: 0x05"))
        self.assertEqual(c.reply(rid, "GATT_READ"), (P.ST_ATT, [5]))
        rid = c.request("GATT_READ", link, 99, 0)
        self.assertEqual(c.reply(rid, "GATT_READ"), (P.ST_ATT, [1]))

    def test_cccd_write_starts_and_stops_notify(self):
        be, store, br, c, link = self.connected()
        rid = c.request("GATT_WRITE", link, 4, P.W_DEFAULT, 0, b"\x01\x00")
        self.assertEqual(c.reply(rid, "GATT_WRITE")[0], P.ST_OK)
        self.assertEqual(be.called("notify"), [("notify", SENSOR, 3, True)])
        self.assertEqual(be.called("write"), [])
        rid = c.request("GATT_READ", link, 4, 0)
        self.assertEqual(c.reply(rid, "GATT_READ"), (P.ST_OK, [b"\x01\x00"]))
        br.value_changed(SENSOR, 3, b"\x00\x50")
        self.assertEqual(c.events("EV_GATT_VALUE"), [[link, 3, b"\x00\x50"]])
        c.request("GATT_WRITE", link, 4, P.W_DEFAULT, 0, b"\x00\x00")
        self.assertEqual(be.called("notify")[-1], ("notify", SENSOR, 3, False))
        br.value_changed(SENSOR, 3, b"\x00\x51")
        self.assertEqual(len(c.events("EV_GATT_VALUE")), 1)

    def test_notify_shared_between_links(self):
        be, store, br, c, link = self.connected()
        st, (link2,) = c.reply(c.request("GATT_OPEN", SENSOR, 1, P.T_LE), "GATT_OPEN")
        c.request("GATT_WRITE", link, 4, P.W_DEFAULT, 0, b"\x01\x00")
        c.request("GATT_WRITE", link2, 4, P.W_DEFAULT, 0, b"\x01\x00")
        self.assertEqual(len(be.called("notify")), 1)
        br.value_changed(SENSOR, 3, b"\x07")
        self.assertEqual(sorted(e[0] for e in c.events("EV_GATT_VALUE")), sorted([link, link2]))
        c.request("GATT_CLOSE", link)
        self.assertEqual(len(be.called("notify")), 1)
        c.request("GATT_CLOSE", link2)

    def test_write_types(self):
        be, store, br, c, link = self.connected()
        rid = c.request("GATT_WRITE", link, 3, P.W_NO_RSP, 0, b"\x01")
        be.answer("write")
        self.assertEqual(c.reply(rid, "GATT_WRITE")[0], P.ST_OK)
        self.assertEqual(be.called("write")[0], ("write", SENSOR, 3, b"\x01", P.W_NO_RSP, 0))
        rid = c.request("GATT_WRITE", link, 3, P.W_PREPARE, 0, b"\x01")
        self.assertEqual(c.reply(rid, "GATT_WRITE")[0], P.ST_UNSUPPORTED)
        rid = c.request("GATT_WRITE", link, 4, P.W_DEFAULT, 0, b"\x01")
        self.assertEqual(c.reply(rid, "GATT_WRITE"), (P.ST_ATT, [0x0d]))

    def test_other_clients_links_are_not_yours(self):
        be, store, br, c, link = self.connected()
        other = FakeClient(br)
        other.request("HELLO", P.VERSION, "x")
        other.request("ENABLE", True)
        for op, args in (("GATT_READ", (link, 3, 0)), ("GATT_CLOSE", (link,))):
            self.assertEqual(other.reply(other.request(op, *args), op)[0], P.ST_NOT_FOUND)

    def test_ops_on_a_connecting_link(self):
        be, store, br, c, link = self.open()
        rid = c.request("GATT_READ", link, 3, 0)
        self.assertEqual(c.reply(rid, "GATT_READ")[0], P.ST_NOT_READY)

    def test_mtu(self):
        be, store, br, c, link = self.connected()
        self.assertEqual(c.reply(c.request("GATT_MTU", link), "GATT_MTU"), (P.ST_OK, [247]))

    def test_adapter_off_drops_links(self):
        be, store, br, c, link = self.connected()
        be.powered = False
        br.adapter_changed({"Powered": False})
        self.assertEqual(c.events("EV_ADAPTER"), [[False, "ztos"]])
        self.assertEqual(c.events("EV_LINK")[-1], [link, P.ST_NOT_READY, False, 0])


class Bonds(unittest.TestCase):
    def test_pair_goes_through_bluez_with_the_agent_hint(self):
        be, store, br, c = make()
        br.device_changed(SENSOR, {"RSSI": -60})
        rid = c.request("BOND_CREATE", SENSOR, P.T_LE)
        self.assertEqual(c.reply(rid, "BOND_CREATE")[0], P.ST_OK)
        self.assertEqual(store.hint, SENSOR)
        self.assertEqual(c.events("EV_BOND_STATE"), [[SENSOR, P.BOND_BONDING, P.ST_OK]])
        br.device_changed(SENSOR, {"Connected": True})      # Pair() connects: ours
        br.device_changed(SENSOR, {"Paired": True, "Bonded": True})
        be.answer("pair")
        self.assertIsNone(store.hint)
        self.assertEqual(c.events("EV_BOND_STATE")[-1], [SENSOR, P.BOND_BONDED, P.ST_OK])
        self.assertEqual(len(c.events("EV_BOND_STATE")), 2)
        self.assertIn(SENSOR, store.android)
        self.assertEqual(be.called("disconnect"), [("disconnect", SENSOR)])  # we connected it for Pair()

    def test_pair_kept_connected_for_a_link(self):
        be, store, br, c = make()
        br.device_changed(SENSOR, {"RSSI": -60})
        c.request("BOND_CREATE", SENSOR, P.T_LE)
        br.device_changed(SENSOR, {"Connected": True})
        st, (link,) = c.reply(c.request("GATT_OPEN", SENSOR, 1, P.T_LE), "GATT_OPEN")
        self.assertEqual(c.events("EV_LINK"), [[link, P.ST_OK, True, 247]])
        be.answer("pair")
        self.assertEqual(be.called("disconnect"), [])
        self.assertEqual(be.called("connect"), [])

    def test_pair_refused(self):
        be, store, br, c = make()
        br.device_changed(SENSOR, {"RSSI": -60})
        c.request("BOND_CREATE", SENSOR, P.T_LE)
        be.answer("pair", error=("org.bluez.Error.AuthenticationRejected", ""))
        self.assertEqual(c.events("EV_BOND_STATE")[-1], [SENSOR, P.BOND_NONE, P.ST_AUTH_FAILED])
        self.assertNotIn(SENSOR, store.android)
        self.assertIsNone(store.hint)

    def test_pair_twice(self):
        be, store, br, c = make()
        br.device_changed(SENSOR, {"RSSI": -60})
        c.request("BOND_CREATE", SENSOR, P.T_LE)
        rid = c.request("BOND_CREATE", SENSOR, P.T_LE)
        self.assertEqual(c.reply(rid, "BOND_CREATE")[0], P.ST_IN_PROGRESS)

    def test_pair_survives_freeze(self):
        be, store, br, c = make()
        br.device_changed(SENSOR, {"RSSI": -60})
        c.request("BOND_CREATE", SENSOR, P.T_LE)
        br.device_changed(SENSOR, {"Connected": True})
        br.set_frozen(True)
        self.assertEqual(be.called("disconnect"), [])   # the person may be answering the sheet

    def test_remove_only_android_made_bonds(self):
        be, store, br, c = make()
        rid = c.request("BOND_REMOVE", LOCK)
        self.assertEqual(c.reply(rid, "BOND_REMOVE")[0], P.ST_NOT_PERMITTED)
        self.assertEqual(be.called("remove"), [])
        store.android.add(LOCK)
        rid = c.request("BOND_REMOVE", LOCK)
        self.assertEqual(c.reply(rid, "BOND_REMOVE")[0], P.ST_OK)
        self.assertEqual(be.called("remove"), [("remove", LOCK)])
        self.assertNotIn(LOCK, store.android)

    def test_never_removes_a_linux_connected_bond(self):
        be, store, br, c = make()
        store.android.add(AIRPODS)
        rid = c.request("BOND_REMOVE", AIRPODS)
        self.assertEqual(c.reply(rid, "BOND_REMOVE")[0], P.ST_NOT_PERMITTED)

    def test_bonds_made_by_linux_are_reported(self):
        be, store, br, c = make()
        br.device_changed(SENSOR, {"Paired": True, "Bonded": True})
        self.assertEqual(c.events("EV_BOND_STATE"), [[SENSOR, P.BOND_BONDED, P.ST_OK]])


class Helpers(unittest.TestCase):
    def test_att_codes(self):
        self.assertEqual(att_code("org.bluez.Error.Failed", "Operation failed with ATT error: 0x0e"), 0x0e)
        self.assertEqual(att_code("org.bluez.Error.NotPermitted", "Read not permitted"), 0x02)
        self.assertEqual(att_code("org.bluez.Error.NotAuthorized", ""), 0x08)
        self.assertIsNone(att_code("org.bluez.Error.Failed", "Not connected"))

    def test_ad_rebuild(self):
        d = {"Name": "nRF", "TxPower": -4, "UUIDs": ["6e400001-b5a3-f393-e0a9-e50e24dcca9e"],
             "ServiceData": {"0000feaa-0000-1000-8000-00805f9b34fb": b"\x10"},
             "AdvertisingData": {0x2a: b"\x01"}}
        ad = build_ad(d)
        self.assertIn(b"\x11\x07" + bytes.fromhex("9ecadc240ee5a9e093f3a3b50100406e"), ad)
        self.assertIn(b"\x04\x09nRF", ad)
        self.assertIn(b"\x02\x0a\xfc", ad)
        self.assertIn(b"\x04\x16\xaa\xfe\x10", ad)
        self.assertTrue(ad.endswith(b"\x02\x2a\x01"))


if __name__ == "__main__":
    unittest.main()
