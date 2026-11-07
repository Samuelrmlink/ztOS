#!/usr/bin/python3
"""A pretend bluetoothd, to take ztos-bt-agent through what the real one does
without a second Bluetooth device: registering, restarting, every question, and a
pairing finishing while a number is on the screen. It never touches bluetoothd.

It owns org.bluez on a private bus, as root, since the agent takes calls only
from root. The agent runs as the user, with DBUS_SYSTEM_BUS_ADDRESS pointing at
that bus, and the real agent stopped: they'd share a socket. See
tests/bt-agent-mock.sh, which arranges all this.

    sudo env DBUS_SESSION_BUS_ADDRESS=ADDRESS python3 bt-agent-mock.py FIFO

Commands come from FIFO, one a line, and what comes back goes to stdout:
    pin | passkey | confirm N | authorize | service UUID
    show-pin P | show-passkey N ENTERED | cancel
    paired | disconnected | trust | reset | discoverable 0|1 | restart | calls | quit
"""
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

DEV = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_02"
PROPS = "org.freedesktop.DBus.Properties"
AGENT = "org.bluez.Agent1"
DEVICE1 = "org.bluez.Device1"
ADAPTER1 = "org.bluez.Adapter1"
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
calls = []
agent_at = [None, None]
values = {
    DEVICE1: {"Alias": "Mock Keyboard", "Address": "AA:BB:CC:DD:EE:02", "Paired": False,
              "Trusted": False, "Connected": True},
    ADAPTER1: {"Discoverable": True},
}


def say(*words):
    print(*words, flush=True)


class Objects(dbus.service.Object):
    @dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return dbus.Dictionary(values[iface], signature="sv")

    @dbus.service.method(PROPS, in_signature="ss", out_signature="v")
    def Get(self, iface, name):
        return values[iface][name]

    @dbus.service.signal(PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, iface, changed, invalidated):
        pass


class Manager(dbus.service.Object):
    @dbus.service.method("org.bluez.AgentManager1", in_signature="os", sender_keyword="sender")
    def RegisterAgent(self, path, capability, sender):
        calls.append("RegisterAgent " + capability)
        agent_at[0], agent_at[1] = sender, path
        say("registered:", capability)

    @dbus.service.method("org.bluez.AgentManager1", in_signature="o")
    def RequestDefaultAgent(self, path):
        calls.append("RequestDefaultAgent")
        say("default agent")


class Device(Objects):
    @dbus.service.method(DEVICE1)
    def CancelPairing(self):
        calls.append("CancelPairing")
        say("CancelPairing called")

    @dbus.service.method(DEVICE1)
    def Disconnect(self):
        calls.append("Disconnect")
        say("Disconnect called")


def ask(method, *args):
    if not agent_at[0]:
        say("no agent yet")
        return
    proxy = dbus.Interface(bus.get_object(agent_at[0], agent_at[1], introspect=False), AGENT)
    getattr(proxy, method)(*args, reply_handler=lambda *r: say(method, "-> ok", *r),
                           error_handler=lambda e: say(method, "->", e.get_dbus_name()), timeout=70)


name = dbus.service.BusName("org.bluez", bus)
manager = Manager(bus, "/org/bluez")
adapter = Objects(bus, "/org/bluez/hci0")
device = Device(bus, DEV)


def back():
    bus.request_name("org.bluez")
    say("name back")
    return False


def command(line):
    w = line.split()
    if not w:
        return
    path = dbus.ObjectPath(DEV)
    cmd = w[0]
    if cmd == "pin":
        ask("RequestPinCode", path)
    elif cmd == "passkey":
        ask("RequestPasskey", path)
    elif cmd == "confirm":
        ask("RequestConfirmation", path, dbus.UInt32(int(w[1])))
    elif cmd == "authorize":
        ask("RequestAuthorization", path)
    elif cmd == "service":
        ask("AuthorizeService", path, w[1])
    elif cmd == "show-pin":
        ask("DisplayPinCode", path, w[1])
    elif cmd == "show-passkey":
        ask("DisplayPasskey", path, dbus.UInt32(int(w[1])), dbus.UInt16(int(w[2])))
    elif cmd == "cancel":
        ask("Cancel")
    elif cmd == "paired":
        values[DEVICE1]["Paired"] = True
        device.PropertiesChanged(DEVICE1, {"Paired": True}, [])
    elif cmd == "disconnected":
        values[DEVICE1]["Connected"] = False
        device.PropertiesChanged(DEVICE1, {"Connected": False}, [])
    elif cmd == "trust":
        values[DEVICE1]["Trusted"] = True
    elif cmd == "reset":
        values[DEVICE1].update(Paired=False, Connected=True, Trusted=False)
    elif cmd == "discoverable":
        values[ADAPTER1]["Discoverable"] = w[1] == "1"
    elif cmd == "restart":
        # as bluetoothd stopping and starting: its name goes, then comes back
        bus.release_name("org.bluez")
        agent_at[0] = None
        say("name released")
        GLib.timeout_add(1000, back)
    elif cmd == "calls":
        say(calls)
    elif cmd == "quit":
        loop.quit()


def ready(fd, _cond):
    for one in os.read(fd, 4096).decode().splitlines():
        command(one)
    return True


# read-write, so it doesn't end when a writer goes
GLib.io_add_watch(os.open(sys.argv[1], os.O_RDWR), GLib.IO_IN, ready)
loop = GLib.MainLoop()
say("mock bluetoothd up")
loop.run()
