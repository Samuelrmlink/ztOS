# SPDX-License-Identifier: GPL-2.0-or-later
"""ztos-btbridge's backend: BlueZ over D-Bus (dbus-python, GLib main loop).

Only BlueZ's public D-Bus API is used: Adapter1, Device1, GattService1,
GattCharacteristic1, GattDescriptor1, ProfileManager1/Profile1. No mgmt or
HCI socket, and nothing that could read keys. The bridge is one more D-Bus
client of bluetoothd, so its discovery filter and its StartDiscovery are its
own: they don't disturb the shell's.

RFCOMM (Android's BluetoothSocket): by UUID, the bridge registers an
org.bluez.Profile1 for that UUID (client role) and calls Device1.ConnectProfile:
BlueZ looks the channel up with SDP, connects with its own security and keys
(pairing, if needed, through its agent), and hands the connected socket to
Profile1.NewConnection. By channel only (apps that call the hidden
createRfcommSocket(int)), the bridge opens the RFCOMM socket itself; the
kernel uses BlueZ's link keys. Either way the RFCOMM socket stays here.
"""
import errno
import os
import re
import socket
import struct

import dbus
import dbus.service
from gi.repository import GLib

from . import proto as P
from .relay import Relay

PROFILE_MANAGER = "org.bluez.ProfileManager1"
PROFILE = "org.bluez.Profile1"
PROFILE_ROOT = "/org/ztos/btbridge/profile"
RFCOMM_TIMEOUT = 40
SOL_BLUETOOTH = 274
BT_SECURITY = 4
BT_SECURITY_LOW, BT_SECURITY_MEDIUM = 1, 2


ADVERTISEMENT = "org.bluez.LEAdvertisement1"
DBUS_PROPS = "org.freedesktop.DBus.Properties"
ADV_ROOT = "/org/ztos/btbridge/adv"


def dbus_adv_props(props):
    """core.adv_props' plain values -> dbus-python values for LEAdvertisement1."""
    out = {}
    for k, v in props.items():
        if k in ("Type", "LocalName"):
            out[k] = dbus.String(v)
        elif k in ("ServiceUUIDs", "Includes"):
            out[k] = dbus.Array(v, signature="s")
        elif k == "ManufacturerData":
            out[k] = dbus.Dictionary({dbus.UInt16(c): dbus.Array(d, signature="y") for c, d in v.items()},
                                     signature="qv")
        elif k == "ServiceData":
            out[k] = dbus.Dictionary({dbus.String(u): dbus.Array(d, signature="y") for u, d in v.items()},
                                     signature="sv")
        elif k == "Data":
            out[k] = dbus.Dictionary({dbus.Byte(t): dbus.Array(d, signature="y") for t, d in v.items()},
                                     signature="yv")
        elif k in ("Appearance", "Timeout"):
            out[k] = dbus.UInt16(v)
        elif k == "Discoverable":
            out[k] = dbus.Boolean(v)
    return out


class _Advertisement(dbus.service.Object):
    """One Android advertising set, as BlueZ reads it."""

    def __init__(self, backend, adv_id, props):
        self.path = "%s%d" % (ADV_ROOT, adv_id)
        super().__init__(backend.bus, self.path)
        self.backend = backend
        self.adv_id = adv_id
        self.props = dbus_adv_props(props)

    @dbus.service.method(DBUS_PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        if interface != ADVERTISEMENT:
            raise dbus.DBusException("no such interface", name="org.freedesktop.DBus.Error.InvalidArgs")
        return dbus.Dictionary(self.props, signature="sv")

    @dbus.service.method(DBUS_PROPS, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        if interface != ADVERTISEMENT or name not in self.props:
            raise dbus.DBusException("no such property", name="org.freedesktop.DBus.Error.InvalidArgs")
        return self.props[name]

    @dbus.service.method(ADVERTISEMENT, in_signature="", out_signature="", sender_keyword="sender")
    def Release(self, sender=None):
        try:
            if sender != self.backend.bus.get_name_owner("org.bluez"):
                return
        except dbus.DBusException:
            return
        self.backend._adv_released(self.adv_id)


class _Profile(dbus.service.Object):
    """Profile1 for one UUID, client role: BlueZ calls it with the connected socket."""

    def __init__(self, backend, uuid, path):
        super().__init__(backend.bus, path)
        self.backend = backend
        self.uuid = uuid

    def _from_bluez(self, sender):
        try:
            return sender == self.backend.bus.get_name_owner("org.bluez")
        except dbus.DBusException:
            return False

    @dbus.service.method(PROFILE, in_signature="oha{sv}", out_signature="", sender_keyword="sender")
    def NewConnection(self, device, fd, props, sender=None):
        fd = fd.take()
        if not self._from_bluez(sender):
            os.close(fd)
            raise dbus.DBusException("not bluetoothd", name="org.bluez.Error.Rejected")
        self.backend._new_connection(self.uuid, str(device), fd)

    @dbus.service.method(PROFILE, in_signature="o", out_signature="", sender_keyword="sender")
    def RequestDisconnection(self, device, sender=None):
        pass    # the stream ends when either side closes its socket

    @dbus.service.method(PROFILE, in_signature="", out_signature="", sender_keyword="sender")
    def Release(self, sender=None):
        if self._from_bluez(sender):
            self.backend.profiles.pop(self.uuid, None)

BUS = "org.bluez"
OM = "org.freedesktop.DBus.ObjectManager"
PROPS = "org.freedesktop.DBus.Properties"
ADAPTER = "org.bluez.Adapter1"
DEVICE = "org.bluez.Device1"
SERVICE = "org.bluez.GattService1"
CHAR = "org.bluez.GattCharacteristic1"
DESC = "org.bluez.GattDescriptor1"
ADV_MANAGER = "org.bluez.LEAdvertisingManager1"
GATT_MANAGER = "org.bluez.GattManager1"

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


def _rfcomm_channel(fd):
    """The channel of a connected RFCOMM socket (BlueZ's NewConnection doesn't say)."""
    try:
        s = socket.socket(fileno=os.dup(fd))
        try:
            return int(s.getpeername()[1])
        finally:
            s.close()
    except (OSError, IndexError, ValueError):
        return 1


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
        self.profiles = {}        # uuid -> _Profile (registered with BlueZ)
        self.rfcomm_pending = {}  # (device path, uuid) -> {"ok", "err", "fd", "replied", "timer"}
        self.advs = {}            # advertiser id -> _Advertisement (registered with BlueZ)
        self.apps = {}            # GATT server service id -> gattapp.App

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

    # ---- RFCOMM ------------------------------------------------------------

    def rfcomm_connect(self, addr, uuid, channel, secure, ok, err):
        """ok(fd, channel) with a connected RFCOMM socket, or err(name, message)."""
        if uuid is None:
            self._rfcomm_raw(addr, channel, secure, ok, err)
            return
        p = self._dev(addr, err)
        if not p:
            return
        key = (p, uuid)
        if key in self.rfcomm_pending:
            err("org.bluez.Error.InProgress", "a connect to this profile is under way")
            return
        entry = {"ok": ok, "err": err, "fd": None, "replied": False, "timer": None}
        self.rfcomm_pending[key] = entry

        def fail(name, msg):
            if self.rfcomm_pending.get(key) is not entry:
                return
            del self.rfcomm_pending[key]
            if entry["timer"] is not None:
                GLib.source_remove(entry["timer"])
            if entry["fd"] is not None:
                os.close(entry["fd"])
            err(name, msg)

        def timeout():
            entry["timer"] = None
            fail("org.bluez.Error.Failed", "no connection from BlueZ")
            return False
        entry["timer"] = GLib.timeout_add_seconds(RFCOMM_TIMEOUT, timeout)

        def replied():
            entry["replied"] = True
            self._rfcomm_done(key)

        def connect():
            self._call(p, DEVICE, "ConnectProfile", [uuid], replied, fail, timeout=RFCOMM_TIMEOUT)
        self._ensure_profile(uuid, connect, fail)

    def _ensure_profile(self, uuid, ok, err):
        if uuid in self.profiles:
            ok()
            return
        path = "%s/%s" % (PROFILE_ROOT, uuid.replace("-", "_"))
        prof = _Profile(self, uuid, path)
        # client role: no SDP record of ours. Encryption: SSP makes it
        # mandatory for RFCOMM anyway; BlueZ pairs (its agent) if it must.
        opts = dbus.Dictionary({"Role": "client", "Name": "ztOS: Android app",
                                "RequireAuthentication": dbus.Boolean(False),
                                "RequireAuthorization": dbus.Boolean(False),
                                "AutoConnect": dbus.Boolean(False)}, signature="sv")

        def done():
            self.profiles[uuid] = prof
            ok()

        def failed(name, msg):
            prof.remove_from_connection()
            if name.endswith("AlreadyExists"):
                err(name, "another program has the profile %s" % uuid)
            else:
                err(name, msg)
        self._call("/org/bluez", PROFILE_MANAGER, "RegisterProfile", [dbus.ObjectPath(path), uuid, opts],
                   done, failed)

    def _new_connection(self, uuid, device, fd):
        entry = self.rfcomm_pending.get((device, uuid))
        if entry is None or entry["fd"] is not None:
            self.log("RFCOMM connection nobody asked for:", device, uuid)
            os.close(fd)
            return
        entry["fd"] = fd
        self._rfcomm_done((device, uuid))

    def _rfcomm_done(self, key):
        entry = self.rfcomm_pending.get(key)
        if entry is None or entry["fd"] is None or not entry["replied"]:
            return
        del self.rfcomm_pending[key]
        if entry["timer"] is not None:
            GLib.source_remove(entry["timer"])
        fd = entry["fd"]
        entry["ok"](fd, _rfcomm_channel(fd))

    def _rfcomm_raw(self, addr, channel, secure, ok, err):
        try:
            s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM | socket.SOCK_CLOEXEC,
                              socket.BTPROTO_RFCOMM)
            s.setsockopt(SOL_BLUETOOTH, BT_SECURITY,
                         struct.pack("BB", BT_SECURITY_MEDIUM if secure else BT_SECURITY_LOW, 0))
            s.setblocking(False)
            rc = s.connect_ex((addr, channel))
        except OSError as e:
            err("org.bluez.Error.Failed", str(e))
            return
        if rc not in (0, errno.EINPROGRESS, errno.EAGAIN):
            s.close()
            err("org.bluez.Error.Failed", os.strerror(rc))
            return
        state = {"watch": None, "timer": None}

        def finish(e):
            for k, remove in (("watch", GLib.source_remove), ("timer", GLib.source_remove)):
                if state[k] is not None:
                    remove(state[k])
                    state[k] = None
            if e:
                s.close()
                err("org.bluez.Error.Failed", os.strerror(e))
            else:
                ok(s.detach(), channel)

        def writable(fd, cond):
            state["watch"] = None
            finish(s.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR))
            return False

        def timeout():
            state["timer"] = None
            finish(errno.ETIMEDOUT)
            return False
        state["watch"] = GLib.io_add_watch(s.fileno(), GLib.PRIORITY_DEFAULT,
                                           GLib.IOCondition(int(GLib.IO_OUT) | int(GLib.IO_ERR) | int(GLib.IO_HUP)),
                                           writable)
        state["timer"] = GLib.timeout_add_seconds(RFCOMM_TIMEOUT, timeout)

    # ---- GATT server ---------------------------------------------------------

    def serve(self, svc_id, tree, ok, err):
        """Register Android's service svc_id as a BlueZ GATT application."""
        if self.adapter_path is None:
            err("org.bluez.Error.NotReady", "no adapter")
            return
        from .gattapp import App
        self._drop_app(svc_id)
        app = App(self, self.core, svc_id, tree)
        self.apps[svc_id] = app

        def failed(name, msg):
            if self.apps.get(svc_id) is app:
                del self.apps[svc_id]
                app.remove()
            err(name, msg)
        self._call(self.adapter_path, GATT_MANAGER, "RegisterApplication",
                   [dbus.ObjectPath(app.root), dbus.Dictionary({}, signature="sv")], ok, failed)

    def unserve(self, svc_id):
        app = self.apps.get(svc_id)
        if app is None:
            return
        if self.adapter_path:
            self._call(self.adapter_path, GATT_MANAGER, "UnregisterApplication", [dbus.ObjectPath(app.root)],
                       lambda: None, lambda n, m: None)
        self._drop_app(svc_id)

    def _drop_app(self, svc_id):
        app = self.apps.pop(svc_id, None)
        if app is not None:
            app.remove()

    def server_value(self, svc_id, handle, value, indicate):
        """A new value for characteristic `handle`: BlueZ notifies (or indicates,
        as each central subscribed) every central that subscribed."""
        app = self.apps.get(svc_id)
        char = app.chars.get(handle) if app else None
        if char is None:
            return False
        char.send(value)
        return True

    def server_peers(self):
        """Connected LE centrals, as far as BlueZ's Device1 objects tell."""
        out = []
        for addr, path in self.dev_path.items():
            d = self.objs.get(path, {}).get(DEVICE, {})
            if d.get("Connected"):
                out.append(addr)
        return out

    def address_of(self, path):
        d = self.objs.get(path, {}).get(DEVICE, {})
        return d.get("Address")

    # ---- LE advertising ----------------------------------------------------

    def advertise(self, adv_id, props, ok, err):
        """Register Android's set adv_id with LEAdvertisingManager1. ok(None):
        BlueZ picks the tx power and doesn't say which (core reports Android's)."""
        if self.adapter_path is None:
            err("org.bluez.Error.NotReady", "no adapter")
            return
        self._drop_adv(adv_id)
        obj = _Advertisement(self, adv_id, props)
        self.advs[adv_id] = obj

        def failed(name, msg):
            if self.advs.get(adv_id) is obj:
                del self.advs[adv_id]
                obj.remove_from_connection()
            err(name, msg)
        self._call(self.adapter_path, ADV_MANAGER, "RegisterAdvertisement",
                   [dbus.ObjectPath(obj.path), dbus.Dictionary({}, signature="sv")],
                   lambda: ok(None), failed)

    def unadvertise(self, adv_id):
        obj = self.advs.get(adv_id)
        if obj is None:
            return
        if self.adapter_path:
            self._call(self.adapter_path, ADV_MANAGER, "UnregisterAdvertisement", [dbus.ObjectPath(obj.path)],
                       lambda: None, lambda n, m: None)
        self._drop_adv(adv_id)

    def _drop_adv(self, adv_id):
        obj = self.advs.pop(adv_id, None)
        if obj is not None:
            try:
                obj.remove_from_connection()
            except LookupError:
                pass

    def _adv_released(self, adv_id):
        self._drop_adv(adv_id)
        if self.core:
            self.core.adv_released(adv_id)

    def relay(self, a, b, prefix, on_close):
        return Relay(a, b, prefix, on_close)

    def unrelay(self, r):
        r.close()

    def after(self, seconds, fn):
        def fire():
            fn()
            return False
        return GLib.timeout_add(int(seconds * 1000), fire)

    def cancel(self, handle):
        GLib.source_remove(handle)
