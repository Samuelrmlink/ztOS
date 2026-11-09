"""The GATT application objects on a private D-Bus (no BlueZ): what
bluetoothd would read and call."""
import os
import shutil
import subprocess
import time
import unittest

from fakes import P  # noqa: F401  (sets sys.path)

try:
    import dbus
    import dbus.mainloop.glib
    from gi.repository import GLib
    from ztbt import gattapp
    from ztbt.gatts import ServerSvc, server_tree
except ImportError:
    dbus = None

HR = "0000180d-0000-1000-8000-00805f9b34fb"
HRM = "00002a37-0000-1000-8000-00805f9b34fb"
CCCD = "00002902-0000-1000-8000-00805f9b34fb"
CTRL = "00002a39-0000-1000-8000-00805f9b34fb"


class FakeCore:
    def __init__(self):
        self.calls = []

    def server_read(self, svc_id, handle, addr, offset, mtu, ok, err):
        self.calls.append(("read", handle, addr, offset, mtu))
        if handle == 2:
            ok(b"\x00\x48")
        else:
            err(0x02)

    def server_write(self, svc_id, handle, addr, value, offset, need_rsp, mtu, ok, err):
        self.calls.append(("write", handle, value, need_rsp))
        ok(b"")

    def server_notify_state(self, svc_id, handle, on, indicate=False):
        self.calls.append(("notify", handle, on, indicate))


class FakeBackend:
    def __init__(self, bus):
        self.bus = bus

    def address_of(self, path):
        return "C0:00:00:00:00:02" if path.endswith("dev_C0_00_00_00_00_02") else None


@unittest.skipIf(dbus is None or not shutil.which("dbus-daemon"), "needs dbus-python and dbus-daemon")
class GattAppTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
        cls.daemon = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--print-address=1"],
                                      stdout=subprocess.PIPE, text=True)
        cls.address = cls.daemon.stdout.readline().strip()

    @classmethod
    def tearDownClass(cls):
        cls.daemon.terminate()
        cls.daemon.wait()

    def setUp(self):
        self.server = dbus.bus.BusConnection(self.address)
        self.client = dbus.bus.BusConnection(self.address)     # plays bluetoothd
        self.core = FakeCore()
        self.backend = FakeBackend(self.server)
        self.orig = gattapp._Obj._from_bluez
        caller = self.client.get_unique_name()
        gattapp._Obj._from_bluez = lambda obj, sender: sender == caller
        svc = ServerSvc(1, None, [(P.DB_PRIMARY, HR, 0, 0), (P.DB_CHARACTERISTIC, HRM, 0x12, 0x01),
                                  (P.DB_DESCRIPTOR, CCCD, 0, 0x11), (P.DB_CHARACTERISTIC, CTRL, 0x0c, 0x10)],
                        [1, 2, 3, 4])
        self.app = gattapp.App(self.backend, self.core, 1, server_tree(svc))

    def tearDown(self):
        self.app.remove()
        gattapp._Obj._from_bluez = self.orig
        self.server.close()
        self.client.close()

    def call(self, path, iface, method, *args):
        """Call asynchronously and spin the main loop until the answer."""
        out = {}
        obj = self.client.get_object(self.server.get_unique_name(), path)
        getattr(dbus.Interface(obj, iface), method)(
            *args, reply_handler=lambda *r: out.setdefault("ok", r), error_handler=lambda e: out.setdefault("err", e))
        ctx = GLib.MainContext.default()
        end = time.monotonic() + 5
        while not out and time.monotonic() < end:
            ctx.iteration(False)
        return out

    def test_managed_objects(self):
        out = self.call(self.app.root, gattapp.OBJECT_MANAGER, "GetManagedObjects")
        objs = out["ok"][0]
        paths = sorted(str(p) for p in objs)
        self.assertEqual(paths, [self.app.root + "/service0", self.app.root + "/service0/char0002",
                                 self.app.root + "/service0/char0004"])           # no CCCD exported
        char = objs[dbus.ObjectPath(self.app.root + "/service0/char0002")][gattapp.GATT_CHAR]
        self.assertEqual(list(char["Flags"]), ["notify", "read"])
        self.assertEqual(str(char["Service"]), self.app.root + "/service0")

    def test_read_write_notify(self):
        dev = dbus.ObjectPath("/org/bluez/hci0/dev_C0_00_00_00_00_02")
        char = self.app.root + "/service0/char0002"
        out = self.call(char, gattapp.GATT_CHAR, "ReadValue",
                        dbus.Dictionary({"device": dev, "offset": dbus.UInt16(0), "mtu": dbus.UInt16(185)},
                                        signature="sv"))
        self.assertEqual(bytes(out["ok"][0]), b"\x00\x48")
        self.assertEqual(self.core.calls[-1], ("read", 2, "C0:00:00:00:00:02", 0, 185))
        out = self.call(self.app.root + "/service0/char0004", gattapp.GATT_CHAR, "ReadValue",
                        dbus.Dictionary({}, signature="sv"))
        self.assertEqual(out["err"].get_dbus_name(), "org.bluez.Error.NotPermitted")
        out = self.call(self.app.root + "/service0/char0004", gattapp.GATT_CHAR, "WriteValue",
                        dbus.Array(b"\x01", signature="y"), dbus.Dictionary({"type": "command"}, signature="sv"))
        self.assertIn("ok", out)
        self.assertEqual(self.core.calls[-1], ("write", 4, b"\x01", False))
        self.call(char, gattapp.GATT_CHAR, "StartNotify")
        self.assertEqual(self.core.calls[-1], ("notify", 2, True, False))

    def test_only_bluetoothd(self):
        gattapp._Obj._from_bluez = lambda obj, sender: False
        out = self.call(self.app.root + "/service0/char0002", gattapp.GATT_CHAR, "ReadValue",
                        dbus.Dictionary({}, signature="sv"))
        self.assertEqual(out["err"].get_dbus_name(), "org.bluez.Error.NotPermitted")
        self.assertEqual(self.core.calls, [])

    def test_value_change_signal(self):
        seen = []
        self.client.add_signal_receiver(lambda iface, changed, inv, path=None: seen.append((path, dict(changed))),
                                        "PropertiesChanged", gattapp.DBUS_PROPS, path_keyword="path")
        self.app.chars[2].send(b"\x00\x50")
        ctx = GLib.MainContext.default()
        end = time.monotonic() + 5
        while not seen and time.monotonic() < end:
            ctx.iteration(False)
        self.assertEqual(seen[0][0], self.app.root + "/service0/char0002")
        self.assertEqual(bytes(seen[0][1]["Value"]), b"\x00\x50")


if __name__ == "__main__":
    unittest.main()
