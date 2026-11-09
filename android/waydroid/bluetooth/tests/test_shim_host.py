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

from fakes import LOCK, SENSOR, FakeBackend, FakeStore, P  # noqa: F401
from ztbt.core import Bridge

HERE = os.path.dirname(os.path.abspath(__file__))
SHIM = os.path.join(HERE, "..", "shim")
WORK = os.environ.get("ZTBT_WORK", os.path.expanduser("~/ztbt-work"))

try:
    from gi.repository import GLib
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


if __name__ == "__main__":
    unittest.main()
