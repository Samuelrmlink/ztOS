"""ztos-btbridge's BlueZ backend: its object bookkeeping, without a bus."""
import unittest

from fakes import CCCD, HR, HRM, P

try:
    import dbus
    from ztbt import bluez as B
except ImportError:
    dbus = None

DEV = "/org/bluez/hci0/dev_C0_00_00_00_00_02"
SENSOR = "C0:00:00:00:00:02"


class FakeCore:
    def __init__(self):
        self.devs = {}
        self.changes = []
        self.values = []
        self.adapter = []

    def device_changed(self, addr, changed, added=False):
        self.changes.append((addr, changed, added))

    def device_removed(self, addr):
        self.changes.append((addr, None, False))

    def value_changed(self, addr, handle, value):
        self.values.append((addr, handle, value))

    def adapter_changed(self, changed):
        self.adapter.append(changed)


def backend():
    b = B.BlueZ.__new__(B.BlueZ)
    b.log = lambda *a: None
    b.core = FakeCore()
    b.objs = {}
    b.adapter_path = None
    b.dev_path = {}
    b.reading = {}
    b.notify_on = set()
    b.our_discovery = False
    b._added("/org/bluez/hci0", {B.ADAPTER: {"Address": "00:B5:D0:3D:D8:44", "Alias": "ztos", "Powered": True,
                                              "Class": dbus.UInt32(0x6c0110)},
                                  B.ADV_MANAGER: {"SupportedInstances": dbus.Byte(5)}}, quiet=True)
    b._added(DEV, {B.DEVICE: {"Address": SENSOR, "AddressType": "random", "Connected": True,
                              "ManufacturerData": dbus.Dictionary({dbus.UInt16(0x59): dbus.Array(b"\x01", signature="y")},
                                                                  signature="qv")}}, quiet=True)
    s = DEV + "/service000a"
    b._added(s, {B.SERVICE: {"UUID": HR, "Primary": True, "Device": DEV}})
    # no Handle properties: from the paths (declaration 0x000b -> value 0x000c)
    b._added(s + "/char000b", {B.CHAR: {"UUID": HRM, "Service": s, "Flags": ["notify", "read"], "MTU": 247}})
    b._added(s + "/char000b/desc000d", {B.DESC: {"UUID": CCCD, "Characteristic": s + "/char000b"}})
    b._added(DEV + "/service0010", {B.SERVICE: {"UUID": "180f", "Primary": True, "Handle": dbus.UInt16(0x10)}})
    return b


@unittest.skipIf(dbus is None, "dbus-python missing")
class BlueZTests(unittest.TestCase):
    def test_plain(self):
        v = B.plain(dbus.Dictionary({dbus.String("a"): dbus.Array([dbus.Byte(1), dbus.Byte(2)], signature="y"),
                                     dbus.String("b"): dbus.Boolean(True)}, signature="sv"))
        self.assertEqual(v, {"a": b"\x01\x02", "b": True})
        self.assertIs(type(v["b"]), bool)

    def test_adapter_and_devices(self):
        b = backend()
        self.assertEqual(b.adapter(), {"Address": "00:B5:D0:3D:D8:44", "Name": "ztos", "Class": 0x6c0110,
                                       "Powered": True, "AdvInstances": 5})
        self.assertEqual(b.devices()[SENSOR]["ManufacturerData"], {0x59: b"\x01"})

    def test_gatt_db(self):
        b = backend()
        db = b.gatt_db(SENSOR)
        self.assertEqual(db, [
            (P.DB_PRIMARY, 0x0a, 0x0a, 0x0f, HR, 0),
            (P.DB_CHARACTERISTIC, 0x0c, 0x0c, 0x0c, HRM, 0x12),
            (P.DB_DESCRIPTOR, 0x0d, 0x0d, 0x0d, CCCD, 0),
            (P.DB_PRIMARY, 0x10, 0x10, 0x10, "0000180f-0000-1000-8000-00805f9b34fb", 0),
        ])
        self.assertEqual(b.attribute(SENSOR, 0x0d), ("desc", CCCD, 0x0c))
        self.assertEqual(b.attribute(SENSOR, 0x0c)[0], "char")
        self.assertIsNone(b.attribute(SENSOR, 0x99))
        self.assertEqual(b.mtu(SENSOR), 247)

    def test_notifications_only_while_notifying_and_not_reading(self):
        b = backend()
        path = DEV + "/service000a/char000b"
        val = dbus.Array(b"\x00\x42", signature="y")
        b._on_props(B.CHAR, {"Value": val}, [], path=path)
        self.assertEqual(b.core.values, [])
        b.notify_on.add(path)
        b.reading[path] = 1                    # the Value change a ReadValue makes
        b._on_props(B.CHAR, {"Value": val}, [], path=path)
        self.assertEqual(b.core.values, [])
        del b.reading[path]
        b._on_props(B.CHAR, {"Value": val}, [], path=path)
        self.assertEqual(b.core.values, [(SENSOR, 0x0c, b"\x00\x42")])

    def test_device_signals(self):
        b = backend()
        b.core.devs[SENSOR] = {"RSSI": -50}
        b._on_props(B.DEVICE, {"Connected": dbus.Boolean(False)}, ["RSSI"], path=DEV)
        self.assertEqual(b.core.changes[-1], (SENSOR, {"Connected": False}, False))
        self.assertNotIn("RSSI", b.core.devs[SENSOR])
        b._on_added(dbus.ObjectPath("/org/bluez/hci0/dev_11_22_33_44_55_66"),
                    {B.DEVICE: {"Address": "11:22:33:44:55:66"}})
        self.assertEqual(b.core.changes[-1][0::2], ("11:22:33:44:55:66", True))
        b._on_removed(dbus.ObjectPath(DEV), [B.DEVICE])
        self.assertEqual(b.core.changes[-1], (SENSOR, None, False))
        self.assertNotIn(SENSOR, b.dev_path)

    def rfcomm(self, reply_first):
        b = backend()
        b.profiles = {}
        b.rfcomm_pending = {}
        calls = []

        def fake_call(path, iface, method, args, ok, err, timeout=None, convert=None):
            calls.append((path, method, args))
            self.reply_ok, self.reply_err = ok, err
        b._call = fake_call
        b._ensure_profile = lambda uuid, ok, err: ok()
        got = []
        b.rfcomm_connect(SENSOR, "00001101-0000-1000-8000-00805f9b34fb", -1, True,
                         lambda fd, ch: got.append((fd, ch)), lambda n, m: got.append((n, m)))
        self.assertEqual(calls, [(DEV, "ConnectProfile", ["00001101-0000-1000-8000-00805f9b34fb"])])
        import socket
        a, s = socket.socketpair()
        if reply_first:
            self.reply_ok()
            self.assertEqual(got, [])
            b._new_connection("00001101-0000-1000-8000-00805f9b34fb", DEV, s.detach())
        else:
            b._new_connection("00001101-0000-1000-8000-00805f9b34fb", DEV, s.detach())
            self.assertEqual(got, [])
            self.reply_ok()
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0][1], 1)
        self.assertEqual(b.rfcomm_pending, {})
        import os
        os.close(got[0][0])
        a.close()
        return b

    def test_rfcomm_reply_then_connection(self):
        self.rfcomm(True)

    def test_rfcomm_connection_then_reply(self):
        self.rfcomm(False)

    def test_rfcomm_failure_closes_a_connection_that_came(self):
        import os
        import socket
        b = backend()
        b.profiles = {}
        b.rfcomm_pending = {}

        def fake_call(path, iface, method, args, ok, err, timeout=None, convert=None):
            self.reply_err = err
        b._call = fake_call
        b._ensure_profile = lambda uuid, ok, err: ok()
        got = []
        b.rfcomm_connect(SENSOR, "00001101-0000-1000-8000-00805f9b34fb", -1, True,
                         lambda fd, ch: got.append("ok"), lambda n, m: got.append(n))
        a, s = socket.socketpair()
        fd = s.detach()
        b._new_connection("00001101-0000-1000-8000-00805f9b34fb", DEV, fd)
        self.reply_err("org.bluez.Error.Failed", "Connection refused")
        self.assertEqual(got, ["org.bluez.Error.Failed"])
        with self.assertRaises(OSError):
            os.fstat(fd)
        a.close()

    def test_unasked_connection_closed(self):
        import os
        import socket
        b = backend()
        b.rfcomm_pending = {}
        a, s = socket.socketpair()
        fd = s.detach()
        b._new_connection("00001101-0000-1000-8000-00805f9b34fb", DEV, fd)
        with self.assertRaises(OSError):
            os.fstat(fd)
        a.close()

    def test_advertisement_props_typed_for_bluez(self):
        v = B.dbus_adv_props({"Type": "peripheral", "LocalName": "zt", "ServiceUUIDs": ["180d"],
                              "ManufacturerData": {0x59: b"\x01"}, "ServiceData": {"feaa": b"\x10"},
                              "Data": {0x2a: b"\x02"}, "Includes": ["tx-power"], "Appearance": 0x341,
                              "Timeout": 3, "Discoverable": True})
        self.assertEqual(v["ManufacturerData"].signature, "qv")
        self.assertIsInstance(list(v["ManufacturerData"].keys())[0], dbus.UInt16)
        self.assertEqual(v["ServiceData"].signature, "sv")
        self.assertEqual(v["Data"].signature, "yv")
        self.assertIsInstance(v["Timeout"], dbus.UInt16)
        self.assertIsInstance(v["Discoverable"], dbus.Boolean)
        self.assertEqual(v["ServiceUUIDs"].signature, "s")

    def test_p21_like_layout_through_core(self):
        """A layout like the P21 printer's (8 Nov): services ff00 and ae30,
        notifying characteristics with CCCDs, the way BlueZ 5.87 exports
        them (Handle = the declaration handle). Every handle in the database
        Android gets must find its object again for reads and CCCD writes; a
        characteristic's is its value handle (declaration + 1), as Fluoride's."""
        b = backend()
        b.profiles, b.rfcomm_pending, b.advs, b.apps = {}, {}, {}, {}
        dev = "/org/bluez/hci0/dev_8C_D0_0D_AB_11_9A"
        addr = "8C:D0:0D:AB:11:9A"
        b._added(dev, {B.DEVICE: {"Address": addr, "AddressType": "public", "Connected": True,
                                  "ServicesResolved": True}}, quiet=True)
        u = "0000%s-0000-1000-8000-00805f9b34fb".__mod__
        layout = [("0001", "ff00", [("0002", "ff01", ["notify", "read"], ["0004"]),
                                     ("0005", "ff02", ["write-without-response", "write"], []),
                                     ("0007", "ff03", ["notify"], ["0009"]),
                                     ("000a", "ff05", ["notify", "write"], ["000c"])]),
                  ("000d", "ae30", [("000e", "ae01", ["write-without-response"], []),
                                     ("0010", "ae02", ["notify"], ["0012"]),
                                     ("0013", "ae3c", ["notify", "write-without-response"], ["0015"])])]
        for sh, suuid, chars in layout:
            sp = "%s/service%s" % (dev, sh)
            b._added(sp, {B.SERVICE: {"UUID": u(suuid), "Primary": True, "Handle": dbus.UInt16(int(sh, 16))}})
            for ch, cuuid, flags, descs in chars:
                cp = "%s/char%s" % (sp, ch)
                b._added(cp, {B.CHAR: {"UUID": u(cuuid), "Service": dbus.ObjectPath(sp), "Flags": flags,
                                       "Handle": dbus.UInt16(int(ch, 16))}})
                for dh in descs:
                    b._added("%s/desc%s" % (cp, dh),
                             {B.DESC: {"UUID": u("2902"), "Characteristic": dbus.ObjectPath(cp),
                                       "Handle": dbus.UInt16(int(dh, 16))}})
        db = b.gatt_db(addr)
        handles = [e[1] for e in db]
        self.assertEqual(len(handles), len(set(handles)))              # unique
        self.assertEqual(len(db), 2 + 7 + 5)
        calls = []
        b._call = lambda path, iface, method, args, ok, err, timeout=None, convert=None: calls.append((path, method))
        for kind, h, start, end, uuid, props in db:
            attr = b.attribute(addr, h)
            self.assertIsNotNone(attr, hex(h))
            if kind == P.DB_CHARACTERISTIC:
                b.read(addr, h, 0, lambda v: None, lambda n, m: None)
                self.assertTrue(calls[-1][0].endswith("/char%04x" % (h - 1)), (hex(h), calls[-1]))
            if kind == P.DB_DESCRIPTOR:
                self.assertEqual(attr[0], "desc")
                b.notify(addr, attr[2], True, lambda: None, lambda n, m: None)   # the CCCD write's parent
                self.assertEqual(calls[-1], ("%s/char%04x" % (calls[-1][0].rsplit("/char", 1)[0], attr[2] - 1),
                                             "StartNotify"))
        ae3c = next(e for e in db if e[4] == u("ae3c"))
        self.assertEqual(ae3c[1], 0x14)                                # its value handle

    def test_device_interfaces(self):
        b = backend()
        self.assertIn(B.DEVICE, b.device_interfaces(SENSOR))
        self.assertIn(B.CHAR, b.device_interfaces(SENSOR))             # its GATT objects
        self.assertNotIn("org.bluez.MediaTransport1", b.device_interfaces(SENSOR))
        b._added(DEV + "/fd0", {"org.bluez.MediaTransport1": {"State": "active"}}, quiet=True)
        self.assertIn("org.bluez.MediaTransport1", b.device_interfaces(SENSOR))
        self.assertEqual(b.device_interfaces("00:00:00:00:00:00"), set())

    def test_other_adapters_devices_ignored(self):
        b = backend()
        b._on_added(dbus.ObjectPath("/org/bluez/hci1/dev_11_22_33_44_55_66"),
                    {B.DEVICE: {"Address": "11:22:33:44:55:66"}})
        self.assertNotIn("11:22:33:44:55:66", b.dev_path)


if __name__ == "__main__":
    unittest.main()
