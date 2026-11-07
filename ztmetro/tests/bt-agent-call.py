#!/usr/bin/python3
"""Call ztos-bt-agent as BlueZ would, to see its sheets on a phone with no device
that asks for them. BlueZ's bus policy lets only root call org.bluez.Agent1, so:

    sudo python3 bt-agent-call.py PID METHOD DEVICE_PATH [ARGUMENT...]

PID is the agent's (systemctl --user show -p MainPID --value ztos-bt-agent).
Prints the reply or the error name, and how long it took. Nothing happens to
the device: no pairing starts, and BlueZ doesn't know about the call. Use a path
with no device behind it, such as /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF, because
"cancel" on a sheet that shows a number makes the agent call CancelPairing and
Disconnect on that path. As another user, this shows the bus refusing the call.

    Cancel and Release take no DEVICE_PATH.
"""
import sys
import time

import dbus

AGENT_PATH = "/org/ztos/BluetoothAgent"
IFACE = "org.bluez.Agent1"


def agent_name(bus, pid):
    daemon = dbus.Interface(bus.get_object("org.freedesktop.DBus", "/org/freedesktop/DBus"),
                            "org.freedesktop.DBus")
    for name in daemon.ListNames():
        if name.startswith(":"):
            try:
                if daemon.GetConnectionUnixProcessID(name) == pid:
                    return name
            except dbus.DBusException:
                pass
    raise SystemExit("no bus name for pid %d" % pid)


def main():
    pid, method = int(sys.argv[1]), sys.argv[2]
    rest = sys.argv[3:]
    bus = dbus.SystemBus()
    agent = dbus.Interface(bus.get_object(agent_name(bus, pid), AGENT_PATH, introspect=False), IFACE)
    args = {
        "RequestPinCode": lambda: (dbus.ObjectPath(rest[0]),),
        "RequestPasskey": lambda: (dbus.ObjectPath(rest[0]),),
        "RequestAuthorization": lambda: (dbus.ObjectPath(rest[0]),),
        "DisplayPinCode": lambda: (dbus.ObjectPath(rest[0]), dbus.String(rest[1])),
        "DisplayPasskey": lambda: (dbus.ObjectPath(rest[0]), dbus.UInt32(int(rest[1])), dbus.UInt16(int(rest[2]))),
        "RequestConfirmation": lambda: (dbus.ObjectPath(rest[0]), dbus.UInt32(int(rest[1]))),
        "AuthorizeService": lambda: (dbus.ObjectPath(rest[0]), dbus.String(rest[1])),
        "Cancel": lambda: (),
        "Release": lambda: (),
    }[method]()
    start = time.monotonic()
    try:
        reply = getattr(agent, method)(*args, timeout=70)
        print("%s -> ok %r (%.1f s)" % (method, reply, time.monotonic() - start), flush=True)
    except dbus.DBusException as error:
        print("%s -> %s (%.1f s)" % (method, error.get_dbus_name(), time.monotonic() - start), flush=True)


if __name__ == "__main__":
    main()
