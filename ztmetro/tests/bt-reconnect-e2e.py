#!/usr/bin/env python3
"""ztos-bt-reconnect itself, against a pretend bluetoothd on a private bus
(DBUS_SYSTEM_BUS_ADDRESS) and a ztos-audiod state file written here. It touches
neither the real BlueZ nor the radio: make check-bt-reconnect-e2e.
Needs dbus-daemon, dbus-python and PyGObject. About 20 s."""
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time

import dbus
import dbus.bus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

HERE = os.path.dirname(os.path.abspath(__file__))
DAEMON = os.path.join(HERE, "../../rootfs/overlay/usr/local/bin/ztos-bt-reconnect")
PROPS = "org.freedesktop.DBus.Properties"
OM = "org.freedesktop.DBus.ObjectManager"
DEVICE1 = "org.bluez.Device1"
ADAPTER1 = "org.bluez.Adapter1"
PODS = "40:DA:5C:62:CA:1B"
CHAM = "E3:19:D9:61:F8:3B"


def dpath(mac):
    return "/org/bluez/hci0/dev_" + mac.replace(":", "_")


calls = []


class Root(dbus.service.Object):
    def __init__(self, bus, objs):
        super().__init__(bus, "/")
        self.objs = objs

    @dbus.service.method(OM, out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        return self.objs


class Device(dbus.service.Object):
    def __init__(self, bus, mac, props):
        super().__init__(bus, dpath(mac))
        self.mac = mac
        self.props = props

    @dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return self.props

    @dbus.service.method(DEVICE1)
    def Connect(self):
        calls.append(("Connect", self.mac))
        self.props["Connected"] = dbus.Boolean(True)
        self.PropertiesChanged(DEVICE1, {"Connected": dbus.Boolean(True)}, [])

    # anything else it might call is recorded, and fails the test
    @dbus.service.method(DEVICE1)
    def Disconnect(self):
        calls.append(("Disconnect", self.mac))

    @dbus.service.method(DEVICE1)
    def Pair(self):
        calls.append(("Pair", self.mac))

    @dbus.service.method(PROPS, in_signature="ssv")
    def Set(self, iface, name, value):
        calls.append(("Set", name))

    @dbus.service.signal(PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, iface, changed, invalidated):
        pass

    @dbus.service.signal(DEVICE1, signature="ss")
    def Disconnected(self, name, message):
        pass

    def drop(self, reason):
        self.props["Connected"] = dbus.Boolean(False)
        self.Disconnected(reason, "test")
        self.PropertiesChanged(DEVICE1, {"Connected": dbus.Boolean(False)}, [])


def on_loop(fn, *args):
    done = threading.Event()
    out = []

    def run():
        out.append(fn(*args))
        done.set()
        return False
    GLib.idle_add(run)
    assert done.wait(5)
    return out[0]


def wait_for(what, check, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if check():
            return
        time.sleep(0.05)
    raise AssertionError("timed out waiting for " + what)


def main():
    tmp = tempfile.mkdtemp(prefix="ztos-bt-reconnect-")
    run = os.path.join(tmp, "run")
    os.makedirs(run)
    address = "unix:path=" + os.path.join(tmp, "bus")
    conf = os.path.join(tmp, "bus.conf")
    with open(conf, "w") as f:
        f.write("""<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>%s</listen><auth>EXTERNAL</auth>
<policy context="default"><allow user="*"/><allow own="*"/><allow send_destination="*"/>
<allow receive_sender="*"/></policy></busconfig>""" % address)
    bus_daemon = subprocess.Popen(["dbus-daemon", "--config-file=" + conf, "--nofork", "--nopidfile"])
    wait_for("the bus", lambda: os.path.exists(os.path.join(tmp, "bus")), 10)
    dbus.mainloop.glib.threads_init()
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    loop = GLib.MainLoop()
    threading.Thread(target=loop.run, daemon=True).start()

    devs = {}

    def bluez_up():
        bus = dbus.bus.BusConnection(address)
        bus.request_name("org.bluez")
        audio = dbus.Array(["0000110b-0000-1000-8000-00805f9b34fb", "0000111e-0000-1000-8000-00805f9b34fb"], signature="s")
        gatt = dbus.Array(["00001801-0000-1000-8000-00805f9b34fb", "6e400001-b5a3-f393-e0a9-e50e24dcca9e"], signature="s")
        objs = {dbus.ObjectPath("/org/bluez/hci0"): {ADAPTER1: {"Powered": dbus.Boolean(True)}}}
        for mac, uuids in ((PODS, audio), (CHAM, gatt)):
            props = dbus.Dictionary({"Address": mac, "UUIDs": uuids, "Connected": dbus.Boolean(True),
                                     "Paired": dbus.Boolean(True), "Trusted": dbus.Boolean(True)}, signature="sv")
            devs[mac] = Device(bus, mac, props)
            objs[dbus.ObjectPath(dpath(mac))] = {DEVICE1: props}
        devs["root"] = Root(bus, objs)
        devs["bus"] = bus
    on_loop(bluez_up)

    def audio(playing):
        out = [{"name": "ztos-speaker", "kind": "speaker", "current": not playing, "playing": False},
               {"name": "bluez_output.%s.1" % PODS.replace(":", "_"), "kind": "bluetooth", "address": PODS,
                "current": playing, "playing": playing}]
        with open(os.path.join(run, "ztos-audio-state.json.new"), "w") as f:
            json.dump({"version": 1, "outputs": out, "apps": []}, f)
        os.rename(os.path.join(run, "ztos-audio-state.json.new"), os.path.join(run, "ztos-audio-state.json"))

    # the pause commands (playerctl, Android's) are recorded here, not run
    paused_log = os.path.join(tmp, "paused")
    wrapper = os.path.join(tmp, "pause-wrapper")
    with open(wrapper, "w") as f:
        f.write("#!/bin/sh\necho \"$*\" >> '%s'\n" % paused_log)
    os.chmod(wrapper, 0o755)

    def paused():
        try:
            return open(paused_log).read().splitlines()
        except OSError:
            return []
    pause_cmds = ["playerctl --all-players pause",
                  "sudo -n /usr/local/libexec/ztos-waydroid-recover media pause"]

    audio(True)
    log = open(os.path.join(tmp, "daemon.log"), "w")
    env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS=address, XDG_RUNTIME_DIR=run,
               ZTOS_BT_RECONNECT_TEST_PAUSE=wrapper)
    daemon = subprocess.Popen([sys.executable, DAEMON], env=env, stderr=log)
    try:
        wait_for("it to start", lambda: "started" in open(os.path.join(tmp, "daemon.log")).read(), 10)
        time.sleep(0.5)
        assert calls == [], calls               # started with everything connected: nothing to do

        # the Chameleon drops (cubridge's link): never touched
        on_loop(devs[CHAM].drop, "org.bluez.Reason.Timeout")
        time.sleep(6)
        assert calls == [] and paused() == [], (calls, paused())

        # the AirPods drop while playing through them: the players pause at once
        # (ROADMAP 8.64), and Connect() about 3 s later, once
        on_loop(devs[PODS].drop, "org.bluez.Reason.Timeout")
        wait_for("the players paused", lambda: len(paused()) == 2, 1.5)
        assert sorted(paused()) == sorted(pause_cmds), paused()
        wait_for("Connect() on the AirPods", lambda: ("Connect", PODS) in calls, 8)
        time.sleep(3)
        assert calls == [("Connect", PODS)], calls
        assert len(paused()) == 2, paused()     # back: nothing more, and never a play

        # paused now, and they drop again: not called back
        audio(False)
        time.sleep(0.5)
        on_loop(devs[PODS].drop, "org.bluez.Reason.Remote")
        time.sleep(6)
        assert calls == [("Connect", PODS)] and len(paused()) == 2, (calls, paused())

        # playing, but disconnected from the phone itself: not called back
        on_loop(lambda: devs[PODS].PropertiesChanged(DEVICE1, {"Connected": dbus.Boolean(True)}, []))
        audio(True)
        time.sleep(0.5)
        on_loop(devs[PODS].drop, "org.bluez.Reason.Local")
        time.sleep(6)
        assert calls == [("Connect", PODS)], calls
        assert len(paused()) == 4, paused()     # ...but paused all the same
        assert set(paused()) == set(pause_cmds), paused()   # pause only, never a play
    finally:
        daemon.send_signal(signal.SIGTERM)
        try:
            daemon.wait(5)
        except subprocess.TimeoutExpired:
            daemon.kill()
        bus_daemon.terminate()
        log.close()
    print(open(os.path.join(tmp, "daemon.log")).read(), end="")
    print("bt-reconnect-e2e: ok")


if __name__ == "__main__":
    main()
