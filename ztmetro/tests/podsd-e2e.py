#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ztos-podsd whole, with neither Bluetooth nor AirPods: a pretend bluetoothd owns org.bluez
on a private bus (DBUS_SYSTEM_BUS_ADDRESS), and pretend pods listen on a unix SEQPACKET
socket (ZTOS_PODSD_TEST_AAP_DIR). Everything is in a temporary directory: runtime dir, home,
bus. The real BlueZ and the radio are not touched, and it opens no window.

The bytes are built from pods/README.md's layouts, not captured. The advert below was
encrypted with Python's `cryptography` (OpenSSL), so it also checks our AES against another's.

    make check-podsd     (needs pods/ztos-podsd, dbus-python and PyGObject: on the phone)
"""
import json
import os
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

HERE = os.path.dirname(os.path.abspath(__file__))
DAEMON = os.path.join(HERE, "..", "pods", "ztos-podsd")
MAC = "AA:BB:CC:DD:EE:FF"
DEV = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"
HCI = "/org/bluez/hci0"
RPA_PATH = "/org/bluez/hci0/dev_70_81_94_0D_FB_AA"		# Core spec's sample: resolves with IRK
STRANGER_PATH = "/org/bluez/hci0/dev_41_02_03_04_05_06"
SERVICE = "74ec2172-0bad-4d01-8f77-997b2be0722a"
IRK = bytes.fromhex("ec0234a357c8ad05341010a60a397d9b")
ENC = bytes.fromhex("102132435465768798a9bacbdcedfe0f")
# left pod sends; left 87 charging, right 76, case 55 charging; zeros for the MAC's suffix
ADVERT = bytes.fromhex("071901242021784f110000e6d281bd9ac710bf53fc1bfc3e18f136")
ADAPTER1, DEVICE1 = "org.bluez.Adapter1", "org.bluez.Device1"
PROPS, OM = "org.freedesktop.DBus.Properties", "org.freedesktop.DBus.ObjectManager"
ALLOWED = {"GetManagedObjects", "SetDiscoveryFilter", "StartDiscovery", "StopDiscovery", "Get", "Connect"}


def hdr(op, *payload):
    return bytes([4, 0, 4, 0, op, 0, *payload])


class Bluez(dbus.service.Object):
    """The little of bluetoothd the daemon talks to, and a list of all it asked."""

    def __init__(self, bus):
        name = dbus.service.BusName("org.bluez", bus)
        super().__init__(bus, "/")
        self.name = name
        self.bus = bus
        self.calls = []
        self.connect_fails = False
        self.adapter = {"Powered": dbus.Boolean(True), "Discovering": dbus.Boolean(False)}
        self.device = {"Address": MAC, "Alias": "Sam's AirPods", "Paired": dbus.Boolean(True),
                       "Connected": dbus.Boolean(False), "Modalias": "bluetooth:v004Cp2024d0100",
                       "UUIDs": dbus.Array([SERVICE], signature="s")}
        self.hci = Adapter(bus, self)
        self.dev = Device(bus, self)

    @dbus.service.method(OM, out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        self.calls.append(("GetManagedObjects",))
        return {HCI: {ADAPTER1: self.adapter}, DEV: {DEVICE1: self.device}}

    @dbus.service.signal(OM, signature="oa{sa{sv}}")
    def InterfacesAdded(self, path, ifaces):
        pass

    @dbus.service.signal(OM, signature="oas")
    def InterfacesRemoved(self, path, ifaces):
        pass


class Adapter(dbus.service.Object):
    def __init__(self, bus, world):
        super().__init__(bus, HCI)
        self.world = world
        self.fail_with = None

    @dbus.service.method(ADAPTER1, in_signature="a{sv}")
    def SetDiscoveryFilter(self, f):
        self.world.calls.append(("SetDiscoveryFilter", str(f.get("Transport"))))

    @dbus.service.method(ADAPTER1)
    def StartDiscovery(self):
        self.world.calls.append(("StartDiscovery",))
        if self.fail_with:
            raise dbus.DBusException(self.fail_with, name=self.fail_with)

    @dbus.service.method(ADAPTER1)
    def StopDiscovery(self):
        self.world.calls.append(("StopDiscovery",))

    # anything else it might try is recorded, and fails the test
    @dbus.service.method(ADAPTER1)
    def RemoveDevice(self):
        self.world.calls.append(("RemoveDevice",))

    @dbus.service.method(PROPS, in_signature="ssv")
    def Set(self, iface, name, value):
        self.world.calls.append(("Set", name))

    @dbus.service.signal(PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, iface, changed, invalidated):
        pass


class Device(dbus.service.Object):
    def __init__(self, bus, world):
        super().__init__(bus, DEV)
        self.world = world

    @dbus.service.method(PROPS, in_signature="ss", out_signature="v")
    def Get(self, iface, name):
        self.world.calls.append(("Get", name))
        return self.world.device[name]

    @dbus.service.method(PROPS, in_signature="ssv")
    def Set(self, iface, name, value):
        self.world.calls.append(("Set", name))

    @dbus.service.method(DEVICE1)
    def Connect(self):
        self.world.calls.append(("Connect",))
        if self.world.connect_fails:
            raise dbus.DBusException("no", name="org.bluez.Error.Failed")
        self.world.device["Connected"] = dbus.Boolean(True)
        self.PropertiesChanged(DEVICE1, {"Connected": dbus.Boolean(True)}, [])

    @dbus.service.method(DEVICE1)
    def Pair(self):
        self.world.calls.append(("Pair",))

    @dbus.service.signal(PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, iface, changed, invalidated):
        pass


def on_loop(fn, *args):
    done = threading.Event()

    def run():
        fn(*args)
        done.set()
        return False
    GLib.idle_add(run)
    assert done.wait(5)


def mfr(data):
    return dbus.Dictionary({dbus.UInt16(0x004c): dbus.ByteArray(data)}, signature="qv", variant_level=1)


def wait(what, check, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        v = check()
        if v:
            return v
        time.sleep(0.05)
    raise AssertionError("timed out waiting for " + what)


class Test:
    def __init__(self, tmp):
        self.tmp = tmp
        self.run = os.path.join(tmp, "run")
        self.home = os.path.join(tmp, "home")
        self.aap = os.path.join(tmp, "aap")
        for d in (self.run, self.home, self.aap):
            os.makedirs(d)
        conf = os.path.join(tmp, "bus.conf")
        self.address = "unix:path=" + os.path.join(tmp, "bus")
        with open(conf, "w") as f:
            f.write("""<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>%s</listen><auth>EXTERNAL</auth>
<policy context="default"><allow user="*"/><allow own="*"/><allow send_destination="*"/>
<allow receive_sender="*"/></policy></busconfig>""" % self.address)
        self.dbus_daemon = subprocess.Popen(["dbus-daemon", "--config-file=" + conf, "--nofork", "--nopidfile"])
        wait("the bus", lambda: os.path.exists(os.path.join(tmp, "bus")))
        dbus.mainloop.glib.threads_init()
        dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
        self.loop = GLib.MainLoop()
        threading.Thread(target=self.loop.run, daemon=True).start()
        self.bus = None
        self.daemon = None
        self.err = open(os.path.join(tmp, "daemon.log"), "w")
        self.screen("on")

    def bluez_up(self):
        def up():
            self.bus = dbus.bus.BusConnection(self.address)
            self.bz = Bluez(self.bus)
        on_loop(up)

    def screen(self, state):
        with open(os.path.join(self.run, "ztmetro.state.new"), "w") as f:
            f.write("points 360 740\nscale 1.00\npixels 1080 2220\nscreen %s\n" % state)
        os.rename(os.path.join(self.run, "ztmetro.state.new"), os.path.join(self.run, "ztmetro.state"))

    def start(self):
        env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS=self.address, XDG_RUNTIME_DIR=self.run, HOME=self.home,
                   ZTOS_PODSD_TEST_AAP_DIR=self.aap, ZTOS_PODSD_TRACE=os.path.join(self.tmp, "trace"))
        env.pop("XDG_DATA_HOME", None)
        env.pop("XDG_STATE_HOME", None)
        self.daemon = subprocess.Popen([DAEMON], env=env, stderr=self.err)

    def stop(self):
        self.daemon.send_signal(signal.SIGTERM)
        assert self.daemon.wait(5) == 0

    def state(self):
        try:
            with open(os.path.join(self.run, "ztos-pods-state.json")) as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def device(self):
        s = self.state()
        return s["devices"][0] if s and s["devices"] else None

    def bar(self):
        try:
            with open(os.path.join(self.run, "ztos-pods")) as f:
                return f.read()
        except OSError:
            return ""

    def tell(self, line):
        """One command; its reply."""
        reply = os.path.join(self.tmp, "reply.sock")
        if os.path.exists(reply):
            os.unlink(reply)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        s.bind(reply)
        s.settimeout(5)
        s.sendto(line.encode(), os.path.join(self.run, "ztos-pods.sock"))
        answer = s.recv(200).decode()
        s.close()
        return answer

    def set_device(self, **props):
        def go():
            self.bz.device.update(props)
            self.bz.dev.PropertiesChanged(DEVICE1, props, [])
        on_loop(go)

    def advert(self, path, data):
        props = {"Address": os.path.basename(path)[4:].replace("_", ":"), "AddressType": "random",
                 "RSSI": dbus.Int16(-50), "ManufacturerData": mfr(data)}
        on_loop(lambda: self.bz.InterfacesAdded(path, {DEVICE1: props}))

    def calls(self, name=None):
        return [c for c in self.bz.calls if name is None or c[0] == name]

    def listen_pods(self):
        path = os.path.join(self.aap, MAC)
        if os.path.exists(path):
            os.unlink(path)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        s.bind(path)
        s.listen(4)
        s.settimeout(15)
        return s

    def close(self):
        if self.daemon and self.daemon.poll() is None:
            self.daemon.kill()
        self.dbus_daemon.kill()
        self.loop.quit()


def main():
    tmp = tempfile.mkdtemp(prefix="podsd-e2e-")
    t = Test(tmp)
    try:
        run(t)
        print("podsd-e2e: ok")
    except BaseException:
        t.err.flush()
        print("--- daemon's log ---", file=sys.stderr)
        print(open(os.path.join(tmp, "daemon.log")).read(), file=sys.stderr)
        raise
    finally:
        t.close()
        shutil.rmtree(tmp, ignore_errors=True)


def run(t):
    # bluetoothd off the bus: it must stay off. Nothing asks for it.
    t.start()
    wait("the first state", t.state)
    assert t.state()["bluetooth"] is False and t.state()["devices"] == []
    t.bluez_up()

    # bluetoothd arrives: the pair shows, not connected, no channel, no keys and so no scan
    wait("the pair", lambda: t.device() and t.device()["in_bluez"])
    d = t.device()
    assert d["mac"] == MAC and d["model"] == "pro2-usbc" and d["link"] == "none" and not d["has_keys"]
    assert d["name"] == "Sam's AirPods" and d["battery"]["left"]["level"] is None
    time.sleep(0.5)
    assert not t.calls("StartDiscovery"), t.calls()
    assert "known=" not in t.bar() and "link=0" in t.bar()

    # an app looks: bursts over LE. A stranger's advert is "nearby", and not ours.
    assert t.tell("scan-hint on") == "ok"
    wait("discovery", lambda: t.calls("StartDiscovery"))
    assert ("SetDiscoveryFilter", "le") in t.calls()
    wait("the burst's reply", lambda: t.state()["scanning"])
    t.advert(STRANGER_PATH, ADVERT)
    wait("nearby", lambda: t.state()["nearby"])
    assert t.state()["nearby"][0]["model_label"] == "AirPods Pro 2 (USB-C)"
    assert t.state()["nearby"][0]["battery"]["left"]["level"] == 80	# cleartext: 10% steps
    assert t.device()["battery"]["left"]["level"] is None
    assert t.tell("scan-hint off") == "ok"
    wait("the burst's end", lambda: t.calls("StopDiscovery"), 8)

    # BlueZ connects them: a channel, and nothing before the handshake is answered
    pods = t.listen_pods()
    t.set_device(Connected=dbus.Boolean(True))
    ch, _ = pods.accept()
    ch.settimeout(5)
    assert ("Get", "Connected") in t.calls()		# asked again before anything could page them
    assert ch.recv(100) == bytes.fromhex("00000400010002000000000000000000")
    ch.setblocking(False)
    time.sleep(0.5)
    try:
        raise AssertionError("sent before the answer: %r" % ch.recv(100))
    except BlockingIOError:
        pass
    ch.settimeout(5)
    assert t.tell("set %s listening_mode anc" % MAC) == "err not-linked"
    ch.send(bytes.fromhex("010004000000010003000000000000000000"))
    assert ch.recv(100) == hdr(0x0f, 0xff, 0xff, 0xff, 0xff)
    assert ch.recv(100) == hdr(0x4d, 0xff, 0, 0, 0, 0, 0, 0, 0)
    assert ch.recv(100) == hdr(0x30, 5, 0)
    wait("the link", lambda: t.device()["link"] == "aap")
    assert not t.device()["settings_known"]

    # the start of a link, in the order the captures have it
    ch.send(hdr(0x04, 3, 2, 1, 70, 2, 1, 4, 1, 80, 2, 1, 8, 1, 0, 4, 1))	# case: a placeholder
    ch.send(hdr(0x08, 2, 0, 1, 0))
    ch.send(hdr(0x06, 0, 2))
    ch.send(hdr(0x31, 2, 1, 0, 16, 0, *IRK[::-1], 4, 0, 16, 0, *ENC))
    info = b"\x02\xf8\x00\x04\x00" + b"\0".join([b"Sam's Pods", b"A3048", b"Apple Inc.", b"SECRETSERIAL", b"90.1", b"90.1",
                                               b"1.0.0", b"com.apple", b"SECRETLEFT", b"SECRETRIGHT", b"9442752"]) + b"\0"
    ch.send(hdr(0x1d, *info))
    ch.send(hdr(0x09, 0x0d, 2, 0, 0, 0))
    ch.send(hdr(0x09, 0x28, 2, 0, 0, 0))
    ch.send(hdr(0x09, 0x25, 1, 0, 0, 0))
    wait("the dump", lambda: t.device()["settings"].get("swipe_volume") == 1)
    d = t.device()
    assert d["battery"]["left"]["level"] == 80 and d["battery"]["right"]["level"] == 70
    assert d["battery"]["case"]["level"] is None		# status 04: no 0%
    assert d["ear"] == {"left": "case", "right": "in_ear"}	# right primary: its place first
    assert d["mode"] == "anc" and d["settings"]["conversation"] == 0 and d["settings_known"]
    assert d["name"] == "Sam's Pods" and d["model_number"] == "A3048" and d["firmware"] == "9A348"
    assert d["has_keys"] and "adaptive" in d["caps"] and "conversation" in d["visible"]

    # the status bar's file, and the keys: 0600, out of the state, out of the trace
    wait("the bar", lambda: "link=1" in t.bar())
    bar = t.bar()
    assert "left=80\n" in bar and "right=70\n" in bar and "known=" + MAC in bar and "case=" not in bar
    keys = os.path.join(t.home, ".local/share/ztos/pods/keys")
    wait("the keys file", lambda: os.path.exists(keys))
    assert stat.S_IMODE(os.stat(keys).st_mode) == 0o600
    assert IRK.hex() in open(keys).read() and ENC.hex() in open(keys).read()
    everything = open(os.path.join(t.run, "ztos-pods-state.json")).read() + bar + open(os.path.join(t.tmp, "trace")).read()
    for secret in (IRK.hex(), IRK[::-1].hex(), ENC.hex(), "SECRET", b"SECRET".hex()):
        assert secret not in everything, secret

    # commands, on the wire
    assert t.tell("set %s listening_mode off" % MAC) == "ok"
    assert ch.recv(100) == hdr(0x09, 0x34, 1, 0, 0, 0)		# the gate, right before it
    assert ch.recv(100) == hdr(0x09, 0x0d, 1, 0, 0, 0)
    wait("the mode, as sent", lambda: t.device()["mode"] == "off")
    ch.send(hdr(0x09, 0x0d, 2, 0, 0, 0))			# a stale report of the old one
    time.sleep(0.4)
    assert t.device()["mode"] == "off"
    assert t.tell("set %s conversation 1" % MAC) == "ok"
    assert ch.recv(100) == hdr(0x09, 0x28, 1, 0, 0, 0)
    assert t.tell("set %s adaptive_strength 50" % MAC) == "ok"
    assert ch.recv(100) == hdr(0x09, 0x2e, 50, 0, 0, 0)
    assert t.tell("set %s conversation 7" % MAC) == "err bad-value"
    assert t.tell("set %s nosuch 1" % MAC) == "err unknown-setting"
    assert t.tell("set 00:00:00:00:00:00 conversation 1") == "err unknown-device"
    assert t.tell("rename %s Sam's New Pods" % MAC) == "ok"
    assert ch.recv(200) == hdr(0x1a, 1, 14, 0) + b"Sam's New Pods"
    wait("the name", lambda: t.device()["name"] == "Sam's New Pods")
    assert t.tell("rename %s " % MAC) in ("err bad-name", "err usage")
    assert t.tell("forget %s" % MAC) == "err connected"

    # with a link up it still listens, for the case; not beside music
    n = len(t.calls("StartDiscovery"))
    assert t.tell("scan-hint on") == "ok"
    wait("a burst beside the link", lambda: len(t.calls("StartDiscovery")) > n, 12)
    wait("the burst's reply", lambda: t.state()["scanning"], 12)
    # the left pod reports, so a broadcast leaves it; the case didn't, so one may set it
    t.advert(RPA_PATH, ADVERT)
    wait("the case, from a broadcast", lambda: t.device()["battery"]["case"]["level"] == 55)
    assert t.device()["battery"]["case"]["source"] == "ble"
    assert t.device()["battery"]["left"]["level"] == 80 and t.device()["battery"]["left"]["source"] == "aap"
    n = len(t.calls("StartDiscovery"))

    # the channel drops with BlueZ still connected: back, after a wait
    ch.close()
    wait("the link down", lambda: t.device()["link"] == "connected")
    ch, _ = pods.accept()
    ch.settimeout(5)
    ch.recv(100)
    ch.send(bytes.fromhex("01000400"))
    ch.recv(100), ch.recv(100), ch.recv(100)
    wait("the link back", lambda: t.device()["link"] == "aap")

    # disconnected: the channel is closed, and the readings stay, with their age
    t.set_device(Connected=dbus.Boolean(False))
    wait("the link gone", lambda: t.device()["link"] == "none")
    assert ch.recv(100) == b""
    d = t.device()
    assert d["battery"]["left"]["level"] == 80 and d["battery"]["left"]["source"] == "aap"
    assert "link=0" in t.bar() and "known=" + MAC in t.bar()

    # THE requirement: battery with no connection, from a broadcast, exact, and ours
    wait("a burst", lambda: len(t.calls("StartDiscovery")) > n, 15)
    wait("the burst's reply", lambda: t.state()["scanning"], 15)
    t.advert(RPA_PATH, ADVERT)
    wait("the broadcast's levels", lambda: t.device()["battery"]["left"]["level"] == 87)
    b = t.device()["battery"]
    assert b["left"] == {"level": 87, "charging": True, "seen": b["left"]["seen"], "source": "ble"}
    assert b["right"]["level"] == 76 and not b["right"]["charging"]
    assert b["case"]["level"] == 55 and b["case"]["charging"]
    assert abs(b["left"]["seen"] - time.time()) < 5

    # a closed-lid silence is only silence: nothing changes
    time.sleep(0.2)
    assert t.device()["battery"]["left"]["level"] == 87

    # the screen goes off: the burst ends, and no more start
    t.tell("scan-hint off")
    t.screen("off")
    time.sleep(0.2)
    t.tell("scan-hint on")	# (kicks the daemon's clock; the screen still says no)
    n = len(t.calls("StartDiscovery"))
    time.sleep(2)
    assert len(t.calls("StartDiscovery")) == n
    t.screen("on")

    # Bluetooth off: the same, and it stays off
    def power(on):
        t.bz.adapter["Powered"] = dbus.Boolean(on)
        t.bz.hci.PropertiesChanged(ADAPTER1, {"Powered": dbus.Boolean(on)}, [])
    on_loop(power, False)
    wait("off", lambda: not t.state()["bluetooth"])
    t.tell("scan-hint on")
    n = len(t.calls("StartDiscovery"))
    time.sleep(2)
    assert len(t.calls("StartDiscovery")) == n
    on_loop(power, True)
    wait("on", lambda: t.state()["bluetooth"])

    # bluetoothd restarts while connected: the channel goes with it, the pair comes back
    t.set_device(Connected=dbus.Boolean(True))
    ch, _ = pods.accept()
    ch.settimeout(5)
    ch.recv(100)
    ch.send(bytes.fromhex("01000400"))
    wait("the link", lambda: t.device()["link"] == "aap")
    ch.recv(100), ch.recv(100), ch.recv(100)

    def bluez_leaves():
        t.bz.device["Connected"] = dbus.Boolean(False)
        t.bus.release_name("org.bluez")
    on_loop(bluez_leaves)
    wait("the link gone", lambda: t.device()["link"] == "none")
    assert not t.device()["in_bluez"]
    assert not t.state()["bluetooth"]
    on_loop(lambda: t.bus.request_name("org.bluez"))
    wait("the pair back", lambda: t.device()["in_bluez"])

    # a person taps "connect": BlueZ is asked, once; a failure says so; nothing is asked otherwise
    assert not t.calls("Connect")
    t.bz.connect_fails = True
    assert t.tell("connect %s" % MAC) == "ok"
    wait("the failure", lambda: t.device()["connect"] == "failed")
    assert len(t.calls("Connect")) == 1 and t.device()["link"] == "none"
    t.bz.connect_fails = False
    assert t.tell("connect %s" % MAC) == "ok"
    ch, _ = pods.accept()
    ch.settimeout(5)
    ch.recv(100)
    ch.send(bytes.fromhex("01000400"))
    wait("the link, from the tap", lambda: t.device()["link"] == "aap")
    assert t.device()["connect"] == "idle" and len(t.calls("Connect")) == 2
    ch.recv(100), ch.recv(100), ch.recv(100)
    t.set_device(Connected=dbus.Boolean(False))
    wait("the link gone", lambda: t.device()["link"] == "none")
    d = t.device()
    assert d["last_mode"] and d["mode"] is None and d["link_seen"] and d["settings"]["conversation"] == 1

    # keys and readings across a restart; a second one will not run beside it
    second = subprocess.run([DAEMON], env=dict(os.environ, XDG_RUNTIME_DIR=t.run, HOME=t.home, DBUS_SYSTEM_BUS_ADDRESS=t.address),
                            stderr=subprocess.PIPE, timeout=5)
    assert b"already running" in second.stderr
    t.stop()
    readings = os.path.join(t.home, ".local/state/ztos/pods/readings")
    assert stat.S_IMODE(os.stat(readings).st_mode) == 0o600
    assert IRK.hex() not in open(readings).read()
    assert "link=0" in t.bar()
    os.unlink(os.path.join(t.run, "ztos-pods-state.json"))
    t.start()
    wait("the pair, from its files", lambda: t.device() and t.device()["battery"]["left"]["level"] == 87)
    d = t.device()
    assert d["has_keys"] and d["link"] == "none" and d["name"] == "Sam's New Pods"
    assert d["settings"]["conversation"] == 1		# what we last set
    # and it reads broadcasts straight away, with no connection since it started
    t.tell("scan-hint on")
    wait("the burst's reply", lambda: t.state()["scanning"], 15)
    t.advert(RPA_PATH, ADVERT[:-1] + bytes([ADVERT[-1] ^ 1]))	# a corrupted block does not open
    wait("the complaint", lambda: b"did not open" in open(os.path.join(t.tmp, "daemon.log"), "rb").read())
    assert t.device()["battery"]["left"]["level"] == 87 and t.device()["battery"]["left"]["seen"] == d["battery"]["left"]["seen"]

    # forgetting: when it isn't connected, keys and readings go
    t.tell("scan-hint off")
    time.sleep(0.1)
    assert t.tell("forget %s" % MAC) == "ok"
    wait("forgotten", lambda: (lambda d: not d or not d["has_keys"])(t.device()))
    assert IRK.hex() not in open(keys).read()

    # through all of it, it asked BlueZ for nothing else: no power, no connect, no pair
    bad = [c for c in t.bz.calls if c[0] not in ALLOWED]
    assert not bad, bad
    t.stop()


if __name__ == "__main__":
    main()
