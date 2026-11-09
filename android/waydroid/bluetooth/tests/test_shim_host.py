"""The shim and the bridge together: shim/tests/host_test.cc (the shim, built
for the PC, driven as Android's JNI drives it) against ztos-btbridge's real
core and socket, with a fake BlueZ that answers at once.

Needs g++ and the pinned headers (shim/fetch.sh); skipped without them."""
import os
import shutil
import subprocess
import tempfile
import time
import unittest

import socket

from fakes import LOCK, PRINTER, SENSOR, SPP, FakeBackend, FakeStore, P  # noqa: F401
from ztbt.core import Bridge

HERE = os.path.dirname(os.path.abspath(__file__))
SHIM = os.path.join(HERE, "..", "shim")
WORK = os.environ.get("ZTBT_WORK", os.path.expanduser("~/ztbt-work"))

try:
    from gi.repository import GLib
    from ztbt.relay import Relay
    from ztbt.server import Server
except ImportError:
    GLib = None


class AutoBackend(FakeBackend):
    """BlueZ that answers every call at once, and plays the radio: a sensor
    advertising while an LE scan runs, a notification once one is enabled."""

    def _later(self, name, args, ok, err):
        self.calls.append((name,) + tuple(args))
        if name == "connect":
            def connected():
                ok()
                self.core.device_changed(args[0], {"Connected": True})
                GLib.timeout_add(20, lambda: (self.core.device_changed(args[0], {"ServicesResolved": True}),
                                              False)[-1])
                return False
            GLib.idle_add(connected)
        elif name == "read":
            if self.attribute(args[0], args[1]) is None:
                err("org.bluez.Error.Failed", "Operation failed with ATT error: 0x01")
            else:
                ok(b"\x00\x48")
        elif name == "write":
            ok()
        elif name == "pair":
            def paired():
                self.core.device_changed(args[0], {"Paired": True, "Bonded": True})
                ok()
                return False
            GLib.timeout_add(30, paired)

    def discover(self, transport, ok, err):
        self.calls.append(("discover", transport))
        ok()
        if transport in (P.T_LE, P.T_AUTO):
            GLib.timeout_add(30, lambda: (self.core.device_changed(
                SENSOR, {"RSSI": -60, "ManufacturerData": {0x59: b"\x01"}}), False)[-1])

    def disconnect(self, addr, ok, err):
        self.calls.append(("disconnect", addr))
        ok()
        GLib.idle_add(lambda: (self.core.device_changed(addr, {"Connected": False}), False)[-1])

    def notify(self, addr, handle, on, ok, err):
        self.calls.append(("notify", addr, handle, on))
        ok()
        if on:
            GLib.timeout_add(30, lambda: (self.core.value_changed(addr, handle, b"\x2a"), False)[-1])

    def rfcomm_connect(self, addr, uuid, channel, secure, ok, err):
        """An SPP printer that sends back what it is sent."""
        self.calls.append(("rfcomm", addr, uuid, channel, secure))
        ours, printer = socket.socketpair()
        self.printers.append(printer)

        def echo(fd, cond):
            try:
                data = printer.recv(65536)
            except OSError:
                data = b""
            if not data:
                printer.close()
                return False
            printer.sendall(data)
            return True
        GLib.io_add_watch(printer.fileno(), GLib.PRIORITY_DEFAULT, GLib.IO_IN | GLib.IO_HUP, echo)
        GLib.idle_add(lambda: (ok(ours.detach(), 1), False)[-1])

    def serve(self, svc_id, tree, ok, err):
        """BlueZ takes the application; then a central (the sensor) reads the
        first characteristic and subscribes to it."""
        self.calls.append(("serve", svc_id, tree))
        GLib.idle_add(lambda: (ok(), False)[-1])
        char = next(n["handle"] for n in tree if n["kind"] == "char")

        def central():
            self.core.server_read(svc_id, char, SENSOR, 0, 185,
                                  lambda v: self.calls.append(("served_read", v)),
                                  lambda e: self.calls.append(("served_error", e)))
            GLib.timeout_add(50, lambda: (self.core.server_notify_state(svc_id, char, True), False)[-1])
            return False
        GLib.timeout_add(50, central)

    def unserve(self, svc_id):
        self.calls.append(("unserve", svc_id))

    def server_value(self, svc_id, handle, value, indicate):
        self.calls.append(("server_value", svc_id, handle, value, indicate))
        return True

    def server_peers(self):
        return [SENSOR]

    def advertise(self, adv_id, props, ok, err):
        self.calls.append(("advertise", adv_id, props))
        GLib.idle_add(lambda: (ok(None), False)[-1])

    def unadvertise(self, adv_id):
        self.calls.append(("unadvertise", adv_id))

    def relay(self, a, b, prefix, on_close):
        self.calls.append(("relay",))
        return Relay(a, b, prefix, on_close)

    def unrelay(self, r):
        r.close()

    def after(self, seconds, fn):
        return GLib.timeout_add(int(seconds * 1000), lambda: (fn(), False)[-1])

    def cancel(self, handle):
        GLib.source_remove(handle)


@unittest.skipIf(GLib is None or not shutil.which("g++") or not os.path.isdir(os.path.join(WORK, "src", "bt")),
                 "needs PyGObject, g++ and shim/fetch.sh's sources")
class ShimHostTest(unittest.TestCase):
    def test_shim_against_bridge(self):
        build = subprocess.run([os.path.join(SHIM, "tests", "host-build.sh")], capture_output=True, text=True)
        self.assertEqual(build.returncode, 0, build.stderr[-3000:])
        binary = build.stdout.strip().splitlines()[-1]

        be = AutoBackend()
        be.printers = []
        store = FakeStore()
        bridge = Bridge(be, store)
        be.core = bridge
        bridge.start()
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "misc", "ztos-btbridge", "bridge.sock")
            server = Server(bridge, path, allow_uid=os.getuid(), container_check=False, freezer=False)
            server.listen()
            proc = subprocess.Popen([binary], env=dict(os.environ, ZTBT_SOCKET=path),
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            ctx = GLib.MainContext.default()
            end = time.monotonic() + 60
            while proc.poll() is None and time.monotonic() < end:
                ctx.iteration(False)
                time.sleep(0.002)
            if proc.poll() is None:
                proc.kill()
            settle = time.monotonic() + 1      # the relays see Android's ends close
            while bridge.streams and time.monotonic() < settle:
                ctx.iteration(False)
                time.sleep(0.002)
            out, err = proc.communicate()
            self.assertEqual(proc.returncode, 0, err[-4000:])
            self.assertIn("host test passed", out)
        # what BlueZ was asked: the sensor connected and let go, notifications on,
        # one pairing through Pair(), the Android-made bond removed, never the lock's
        self.assertIn(("connect", SENSOR), be.calls)
        self.assertIn(("disconnect", SENSOR), be.calls)
        self.assertIn(("notify", SENSOR, 3, True), be.calls)
        self.assertIn(("pair", SENSOR), be.calls)
        self.assertIn(("remove", SENSOR), be.calls)
        self.assertNotIn(("remove", LOCK), be.calls)
        self.assertFalse([c for c in be.calls if c[0] == "connect" and c[1] != SENSOR])
        # GATT server: BlueZ got the service without the CCCD; the central's read
        # was answered with Android's value; Android's notification went out
        (_, svc_id, tree), = [c for c in be.calls if c[0] == "serve"]
        self.assertEqual([n["kind"] for n in tree], ["service", "char", "char"])
        self.assertIn(("served_read", b"\x00\x48"), be.calls)
        self.assertIn(("server_value", svc_id, 2, b"\x00\x50", False), be.calls)
        self.assertIn(("unserve", svc_id), be.calls)
        self.assertEqual(bridge.services, {})
        # advertising: one set, its fields read back from Android's AD, then stopped
        advs = [c for c in be.calls if c[0] == "advertise"]
        self.assertEqual(len(advs), 2)                     # started, and again after enable off/on
        self.assertEqual(advs[0][2]["LocalName"], "zt-test")
        self.assertEqual(advs[0][2]["ServiceUUIDs"], ["0000180d-0000-1000-8000-00805f9b34fb"])
        self.assertEqual(advs[0][2]["Type"], "peripheral")
        self.assertEqual(bridge.advs, {})
        # RFCOMM: the printer by its SPP UUID, through a relay; never the AirPods
        self.assertIn(("rfcomm", PRINTER, SPP, -1, True), be.calls)
        self.assertFalse([c for c in be.calls if c[0] == "rfcomm" and c[1] != PRINTER])
        self.assertEqual(bridge.streams, {})


if __name__ == "__main__":
    unittest.main()
