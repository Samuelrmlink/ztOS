# SPDX-License-Identifier: GPL-2.0-or-later
"""Android's GATT services as BlueZ GATT applications (D-Bus objects).

One application per Android service: an ObjectManager root, the GattService1,
its GattCharacteristic1s and GattDescriptor1s. bluetoothd reads them with
GetManagedObjects when the bridge calls GattManager1.RegisterApplication, and
calls ReadValue/WriteValue/StartNotify/StopNotify on them; only calls from
bluetoothd's bus name are taken. The answers come from the Android app
through the core (gatts.py), asynchronously.
"""
import dbus
import dbus.service

GATT_SERVICE = "org.bluez.GattService1"
GATT_CHAR = "org.bluez.GattCharacteristic1"
GATT_DESC = "org.bluez.GattDescriptor1"
OBJECT_MANAGER = "org.freedesktop.DBus.ObjectManager"
DBUS_PROPS = "org.freedesktop.DBus.Properties"
APP_ROOT = "/org/ztos/btbridge/gatt/app"

ATT_ERRORS = {   # Android's ATT status -> BlueZ's D-Bus error (which BlueZ turns back into ATT)
    0x02: "org.bluez.Error.NotPermitted",
    0x03: "org.bluez.Error.NotPermitted",
    0x05: "org.bluez.Error.NotAuthorized",
    0x07: "org.bluez.Error.InvalidOffset",
    0x08: "org.bluez.Error.NotAuthorized",
    0x0d: "org.bluez.Error.InvalidValueLength",
    0x06: "org.bluez.Error.NotSupported",
}


def att_error(code):
    return dbus.DBusException("ATT error 0x%02x" % code, name=ATT_ERRORS.get(code, "org.bluez.Error.Failed"))


class _Obj(dbus.service.Object):
    iface = None

    def __init__(self, app, path, props):
        super().__init__(app.backend.bus, path)
        self.app = app
        self.path = path
        self.props = props

    def _from_bluez(self, sender):
        try:
            return sender == self.app.backend.bus.get_name_owner("org.bluez")
        except dbus.DBusException:
            return False

    @dbus.service.method(DBUS_PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        if interface != self.iface:
            raise dbus.DBusException("no such interface", name="org.freedesktop.DBus.Error.InvalidArgs")
        return self.props

    @dbus.service.method(DBUS_PROPS, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        if interface != self.iface or name not in self.props:
            raise dbus.DBusException("no such property", name="org.freedesktop.DBus.Error.InvalidArgs")
        return self.props[name]

    @dbus.service.signal(DBUS_PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    def _options(self, options):
        dev = options.get("device")
        addr = self.app.backend.address_of(str(dev)) if dev else None
        return addr, int(options.get("offset", 0)), int(options.get("mtu", 0)), str(options.get("type", ""))

    def _read(self, options, sender, reply, error):
        if not self._from_bluez(sender):
            error(dbus.DBusException("not bluetoothd", name="org.bluez.Error.NotPermitted"))
            return
        addr, offset, mtu, _ = self._options(options)
        self.app.core.server_read(self.app.svc_id, self.handle, addr or "00:00:00:00:00:00", offset, mtu,
                                  lambda v: reply(dbus.Array(v, signature="y")),
                                  lambda code: error(att_error(code)))

    def _write(self, value, options, sender, reply, error):
        if not self._from_bluez(sender):
            error(dbus.DBusException("not bluetoothd", name="org.bluez.Error.NotPermitted"))
            return
        addr, offset, mtu, wtype = self._options(options)
        self.app.core.server_write(self.app.svc_id, self.handle, addr or "00:00:00:00:00:00", bytes(value),
                                   offset, wtype != "command", mtu, lambda _v: reply(),
                                   lambda code: error(att_error(code)))


class _Service(_Obj):
    iface = GATT_SERVICE


class _Char(_Obj):
    iface = GATT_CHAR

    @dbus.service.method(GATT_CHAR, in_signature="a{sv}", out_signature="ay", sender_keyword="sender",
                         async_callbacks=("reply", "error"))
    def ReadValue(self, options, sender=None, reply=None, error=None):
        self._read(options, sender, reply, error)

    @dbus.service.method(GATT_CHAR, in_signature="aya{sv}", out_signature="", sender_keyword="sender",
                         async_callbacks=("reply", "error"))
    def WriteValue(self, value, options, sender=None, reply=None, error=None):
        self._write(value, options, sender, reply, error)

    @dbus.service.method(GATT_CHAR, in_signature="", out_signature="", sender_keyword="sender")
    def StartNotify(self, sender=None):
        if self._from_bluez(sender):
            self.props["Notifying"] = dbus.Boolean(True)
            self.app.core.server_notify_state(self.app.svc_id, self.handle, True, self.indicate_only)

    @dbus.service.method(GATT_CHAR, in_signature="", out_signature="", sender_keyword="sender")
    def StopNotify(self, sender=None):
        if self._from_bluez(sender):
            self.props["Notifying"] = dbus.Boolean(False)
            self.app.core.server_notify_state(self.app.svc_id, self.handle, False, self.indicate_only)

    @dbus.service.method(GATT_CHAR, in_signature="", out_signature="", sender_keyword="sender")
    def Confirm(self, sender=None):
        pass       # an indication was confirmed; Android was told when it was sent

    def send(self, value):
        self.props["Value"] = dbus.Array(value, signature="y")
        self.PropertiesChanged(GATT_CHAR, {"Value": self.props["Value"]}, [])


class _Desc(_Obj):
    iface = GATT_DESC

    @dbus.service.method(GATT_DESC, in_signature="a{sv}", out_signature="ay", sender_keyword="sender",
                         async_callbacks=("reply", "error"))
    def ReadValue(self, options, sender=None, reply=None, error=None):
        self._read(options, sender, reply, error)

    @dbus.service.method(GATT_DESC, in_signature="aya{sv}", out_signature="", sender_keyword="sender",
                         async_callbacks=("reply", "error"))
    def WriteValue(self, value, options, sender=None, reply=None, error=None):
        self._write(value, options, sender, reply, error)


class App(dbus.service.Object):
    """One Android service: the ObjectManager root and its objects."""

    def __init__(self, backend, core, svc_id, tree):
        self.root = "%s%d" % (APP_ROOT, svc_id)
        super().__init__(backend.bus, self.root)
        self.backend = backend
        self.core = core
        self.svc_id = svc_id
        self.objs = []
        self.chars = {}
        by_handle = {}
        for node in tree:
            if node["kind"] == "service":
                path = self.root + "/service0"
                props = {"UUID": dbus.String(node["uuid"]), "Primary": dbus.Boolean(node["primary"])}
                obj = _Service(self, path, props)
            elif node["kind"] == "char":
                path = "%s/char%04x" % (by_handle[node["parent"]].path, node["handle"])
                props = {"UUID": dbus.String(node["uuid"]),
                         "Service": dbus.ObjectPath(by_handle[node["parent"]].path),
                         "Flags": dbus.Array(node["flags"], signature="s"),
                         "Notifying": dbus.Boolean(False)}
                obj = _Char(self, path, props)
                obj.indicate_only = "indicate" in node["flags"] and "notify" not in node["flags"]
                self.chars[node["handle"]] = obj
            else:
                path = "%s/desc%04x" % (by_handle[node["parent"]].path, node["handle"])
                props = {"UUID": dbus.String(node["uuid"]),
                         "Characteristic": dbus.ObjectPath(by_handle[node["parent"]].path),
                         "Flags": dbus.Array(node["flags"], signature="s")}
                obj = _Desc(self, path, props)
            obj.handle = node["handle"]
            by_handle[node["handle"]] = obj
            self.objs.append(obj)

    @dbus.service.method(OBJECT_MANAGER, in_signature="", out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        return {dbus.ObjectPath(o.path): {o.iface: o.props} for o in self.objs}

    def remove(self):
        for o in self.objs:
            try:
                o.remove_from_connection()
            except LookupError:
                pass
        try:
            self.remove_from_connection()
        except LookupError:
            pass
