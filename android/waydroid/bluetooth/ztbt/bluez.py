# SPDX-License-Identifier: GPL-2.0-or-later
"""ztos-btbridge's backend: BlueZ over D-Bus (dbus-python, GLib main loop).

Only BlueZ's public D-Bus API is used: Adapter1, Device1, GattService1,
GattCharacteristic1, GattDescriptor1. No mgmt or HCI socket, and nothing that
could read keys. The bridge is one more D-Bus client of bluetoothd, so its
discovery filter and its StartDiscovery are its own: they don't disturb the
shell's.
"""
import re

import dbus
from gi.repository import GLib

from . import proto as P

BUS = "org.bluez"
OM = "org.freedesktop.DBus.ObjectManager"
PROPS = "org.freedesktop.DBus.Properties"
ADAPTER = "org.bluez.Adapter1"
DEVICE = "org.bluez.Device1"
SERVICE = "org.bluez.GattService1"
CHAR = "org.bluez.GattCharacteristic1"
DESC = "org.bluez.GattDescriptor1"
ADV_MANAGER = "org.bluez.LEAdvertisingManager1"

CONNECT_TIMEOUT = 40
CALL_TIMEOUT = 30

# Android's characteristic property bits, from BlueZ's Flags
FLAG_BITS = {"broadcast": 0x01, "read": 0x02, "write-without-response": 0x04, "write": 0x08,
             "notify": 0x10, "indicate": 0x20, "authenticated-signed-writes": 0x40,
             "extended-properties": 0x80}
_HANDLE_IN_PATH = re.compile(r"(service|char|desc)([0-9a-fA-F]{4})$")


def plain(v):
    """dbus-python values -> plain Python."""
    if isinstance(v, dbus.Boolean):
        return bool(v)
    if isinstance(v, (dbus.Byte, dbus.Int16, dbus.UInt16, dbus.Int32, dbus.UInt32, dbus.Int64, dbus.UInt64)):
        return int(v)
    if isinstance(v, (dbus.String, dbus.ObjectPath, dbus.Signature)):
        return str(v)
    if isinstance(v, dbus.Array):
        if v.signature == "y":
            return bytes(v)
        return [plain(x) for x in v]
    if isinstance(v, dict):
        return {plain(k): plain(x) for k, x in v.items()}
    if isinstance(v, (dbus.Struct, tuple)):
        return tuple(plain(x) for x in v)
    if isinstance(v, list):
        return [plain(x) for x in v]
    return v


def _err(e):
    return (e.get_dbus_name() or "org.bluez.Error.Failed", e.get_dbus_message() or "")


class BlueZ:
    def __init__(self, log=None):
        self.log = log or (lambda *a: None)
        self.bus = dbus.SystemBus()
        self.core = None
        self.objs = {}            # path -> {interface: props}
        self.adapter_path = None
        self.dev_path = {}        # address -> path
        self.reading = {}         # characteristic path -> pending reads
        self.notify_on = set()    # characteristic paths we StartNotify'd
        self.our_discovery = False

    # ---- objects ----------------------------------------------------------

    def attach(self, core):
        self.core = core
        om = dbus.Interface(self.bus.get_object(BUS, "/"), OM)
        for path, ifaces in om.GetManagedObjects().items():
            self._added(str(path), ifaces, quiet=True)
        self.bus.add_signal_receiver(self._on_added, "InterfacesAdded", OM, BUS)
        self.bus.add_signal_receiver(self._on_removed, "InterfacesRemoved", OM, BUS)
        self.bus.add_signal_receiver(self._on_props, "PropertiesChanged", PROPS, BUS, path_keyword="path")
        if self.adapter_path is None:
            self.log("no Bluetooth adapter yet")

    def _added(self, path, ifaces, quiet=False):
        entry = self.objs.setdefault(path, {})
        for iface, props in ifaces.items():
            entry[str(iface)] = plain(props)
        if ADAPTER in ifaces and self.adapter_path is None:
            self.adapter_path = path
        if DEVICE in ifaces:
            props = entry[DEVICE]
            if not path.startswith((self.adapter_path or "") + "/"):
                return
            addr = props.get("Address")
            if addr:
                self.dev_path[addr] = path
                if not quiet and self.core:
                    self.core.device_changed(addr, dict(props), added=True)

    def _on_added(self, path, ifaces):
        self._added(str(path), ifaces)

    def _on_removed(self, path, ifaces):
        path = str(path)
        entry = self.objs.get(path, {})
        if DEVICE in ifaces and DEVICE in entry:
            addr = entry[DEVICE].get("Address")
            if addr and self.dev_path.get(addr) == path:
                del self.dev_path[addr]
                if self.core:
                    self.core.device_removed(addr)
        for iface in ifaces:
            entry.pop(str(iface), None)
        if not entry:
            self.objs.pop(path, None)
            self.notify_on.discard(path)

    def _on_props(self, iface, changed, invalidated, path=None):
        iface, path = str(iface), str(path)
        entry = self.objs.get(path)
        if entry is None or iface not in entry:
            return
        changed = plain(changed)
        entry[iface].update(changed)
        for name in invalidated:
            entry[iface].pop(str(name), None)
        if not self.core:
            return
        if iface == DEVICE:
            addr = entry[iface].get("Address")
            if addr and self.dev_path.get(addr) == path:
                gone = {str(n): None for n in invalidated}
                for k in gone:      # properties BlueZ dropped (RSSI after discovery)
                    self.core.devs.get(addr, {}).pop(k, None)
                if changed:
                    self.core.device_changed(addr, changed)
        elif iface == ADAPTER and path == self.adapter_path:
            if "Discovering" in changed and not changed["Discovering"]:
                self.our_discovery = False
            self.core.adapter_changed(changed)
        elif iface == CHAR and "Value" in changed:
            if path in self.notify_on and not self.reading.get(path):
                dev = self._device_of(path)
                h = self._handle(path, CHAR)
                if dev and h is not None:
                    self.core.value_changed(dev, h, bytes(changed["Value"]))

    def _device_of(self, path):
        for addr, p in self.dev_path.items():
            if path.startswith(p + "/"):
                return addr
        return None

    def _handle(self, path, iface):
        props = self.objs.get(path, {}).get(iface, {})
        if "Handle" in props and props["Handle"]:
            return int(props["Handle"])
        m = _HANDLE_IN_PATH.search(path)
        if not m:
            return None
        h = int(m.group(2), 16)
        return h + 1 if iface == CHAR else h    # a path names the declaration; Android wants the value

    def _iface(self, path, iface):
        return dbus.Interface(self.bus.get_object(BUS, path), iface)

    def _call(self, path, iface, method, args, ok, err, timeout=CALL_TIMEOUT, convert=None):
        def done(*result):
            if convert:
                ok(convert(*result))
            else:
                ok()

        def failed(e):
            err(*_err(e))
        try:
            getattr(self._iface(path, iface), method)(*args, reply_handler=done, error_handler=failed,
                                                      timeout=timeout)
        except dbus.DBusException as e:
            err(*_err(e))

    # ---- the core's interface ---------------------------------------------

    def adapter(self):
        if self.adapter_path is None:
            return {"Powered": False}
        a = self.objs[self.adapter_path].get(ADAPTER, {})
        adv = self.objs[self.adapter_path].get(ADV_MANAGER, {})
        return {"Address": a.get("Address", "00:00:00:00:00:00"), "Name": a.get("Alias") or a.get("Name", ""),
                "Class": a.get("Class", 0), "Powered": bool(a.get("Powered")),
                "AdvInstances": adv.get("SupportedInstances", 0)}

    def devices(self):
        return {addr: dict(self.objs[p][DEVICE]) for addr, p in self.dev_path.items()}

    def discover(self, transport, ok, err):
        if self.adapter_path is None:
            err("org.bluez.Error.NotReady", "no adapter")
            return
        if transport is None:
            if not self.our_discovery:
                ok()
                return
            self.our_discovery = False

            def stopped_err(name, msg):
                ok() if name.endswith("Failed") or name.endswith("NotReady") else err(name, msg)
            self._call(self.adapter_path, ADAPTER, "StopDiscovery", [], ok, stopped_err)
            return
        filt = dbus.Dictionary({"Transport": {P.T_LE: "le", P.T_BREDR: "bredr"}.get(transport, "auto"),
                                "DuplicateData": dbus.Boolean(True)}, signature="sv")

        def start():
            if self.our_discovery:
                ok()
                return

            def started():
                self.our_discovery = True
                ok()

            def start_err(name, msg):
                if name.endswith("InProgress"):
                    started()
                else:
                    err(name, msg)
            self._call(self.adapter_path, ADAPTER, "StartDiscovery", [], started, start_err)
        self._call(self.adapter_path, ADAPTER, "SetDiscoveryFilter", [filt], start, err)

    def _dev(self, addr, err):
        p = self.dev_path.get(addr)
        if p is None:
            err("org.bluez.Error.DoesNotExist", addr)
        return p

    def connect(self, addr, ok, err):
        p = self._dev(addr, err)
        if p:
            self._call(p, DEVICE, "Connect", [], ok, err, timeout=CONNECT_TIMEOUT)

    def disconnect(self, addr, ok, err):
        p = self._dev(addr, err)
        if p:
            self._call(p, DEVICE, "Disconnect", [], ok, err)

    def pair(self, addr, ok, err):
        p = self._dev(addr, err)
        if p:
            self._call(p, DEVICE, "Pair", [], ok, err, timeout=90)

    def cancel_pair(self, addr):
        p = self.dev_path.get(addr)
        if p:
            self._call(p, DEVICE, "CancelPairing", [], lambda: None, lambda n, m: None)

    def remove(self, addr, ok, err):
        p = self._dev(addr, err)
        if p and self.adapter_path:
            self._call(self.adapter_path, ADAPTER, "RemoveDevice", [dbus.ObjectPath(p)], ok, err)

    def _gatt(self, addr):
        """handle -> (kind, uuid, path, parent char handle, props) for addr's GATT objects."""
        base = self.dev_path.get(addr)
        out = {}
        if base is None:
            return out
        for path, ifaces in self.objs.items():
            if not path.startswith(base + "/"):
                continue
            for iface, kind in ((SERVICE, "service"), (CHAR, "char"), (DESC, "desc")):
                if iface in ifaces:
                    h = self._handle(path, iface)
                    if h is None:
                        continue
                    parent = None
                    if kind == "desc":
                        cp = ifaces[iface].get("Characteristic")
                        parent = self._handle(cp, CHAR) if cp else None
                    out[h] = (kind, ifaces[iface].get("UUID", ""), path, parent, ifaces[iface])
        return out

    def gatt_db(self, addr):
        attrs = self._gatt(addr)
        services = sorted((h, a) for h, a in attrs.items() if a[0] == "service")
        db = []
        for i, (sh, (_, uuid, spath, _, props)) in enumerate(services):
            children = sorted((h, a) for h, a in attrs.items()
                              if a[0] != "service" and a[2].startswith(spath + "/"))
            end = children[-1][0] if children else sh
            if i + 1 < len(services):
                end = max(end, services[i + 1][0] - 1)
            kind = P.DB_PRIMARY if props.get("Primary", True) else P.DB_SECONDARY
            db.append((kind, sh, sh, end, P.full_uuid(uuid), 0))
            for h, (k, u, _, _, cprops) in children:
                if k == "char":
                    bits = 0
                    for f in cprops.get("Flags", []):
                        bits |= FLAG_BITS.get(f, 0)
                    db.append((P.DB_CHARACTERISTIC, h, h, h, P.full_uuid(u), bits))
                else:
                    db.append((P.DB_DESCRIPTOR, h, h, h, P.full_uuid(u), 0))
        return db

    def attribute(self, addr, handle):
        a = self._gatt(addr).get(handle)
        return (a[0], a[1], a[3]) if a else None

    def read(self, addr, handle, offset, ok, err):
        a = self._gatt(addr).get(handle)
        if a is None or a[0] == "service":
            err("org.bluez.Error.Failed", "ATT error: 0x01")
            return
        path, iface = a[2], CHAR if a[0] == "char" else DESC
        self.reading[path] = self.reading.get(path, 0) + 1

        def done(value):
            self._read_done(path)
            ok(bytes(value))

        def failed(name, msg):
            self._read_done(path)
            err(name, msg)
        self._call(path, iface, "ReadValue", [dbus.Dictionary({"offset": dbus.UInt16(offset)}, signature="sv")],
                   done, failed, convert=lambda v: v)

    def _read_done(self, path):
        # BlueZ updates Value after a read: let that PropertiesChanged go by first
        def later():
            n = self.reading.get(path, 1) - 1
            if n > 0:
                self.reading[path] = n
            else:
                self.reading.pop(path, None)
            return False
        GLib.idle_add(later)

    def write(self, addr, handle, value, wtype, offset, ok, err):
        a = self._gatt(addr).get(handle)
        if a is None or a[0] == "service":
            err("org.bluez.Error.Failed", "ATT error: 0x01")
            return
        opts = {"offset": dbus.UInt16(offset)}
        if a[0] == "char":
            opts["type"] = "command" if wtype == P.W_NO_RSP else "request"
        self._call(a[2], CHAR if a[0] == "char" else DESC, "WriteValue",
                   [dbus.Array(value, signature="y"), dbus.Dictionary(opts, signature="sv")], ok, err)

    def notify(self, addr, handle, on, ok, err):
        a = self._gatt(addr).get(handle)
        if a is None or a[0] != "char":
            err("org.bluez.Error.Failed", "ATT error: 0x01")
            return
        path = a[2]

        def done():
            (self.notify_on.add if on else self.notify_on.discard)(path)
            ok()

        def failed(name, msg):
            if not on:
                self.notify_on.discard(path)
                ok()
            else:
                err(name, msg)
        self._call(path, CHAR, "StartNotify" if on else "StopNotify", [], done, failed)

    def mtu(self, addr):
        for h, a in self._gatt(addr).items():
            if a[0] == "char" and a[4].get("MTU"):
                return int(a[4]["MTU"])
        return 23

    def after(self, seconds, fn):
        def fire():
            fn()
            return False
        return GLib.timeout_add(int(seconds * 1000), fire)

    def cancel(self, handle):
        GLib.source_remove(handle)
