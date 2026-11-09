# SPDX-License-Identifier: GPL-2.0-or-later
"""ztos-btbridge's core: Android's requests, BlueZ's state, and the policy.

Nothing here talks to D-Bus or sockets. The backend (bluez.py, or a fake in
the tests) does BlueZ; a client object (server.py, or a fake) carries packets
to and from one Android Bluetooth stack.

The backend gives the core plain Python values: devices are dicts keyed by
BlueZ's Device1 property names (Address, AddressType, Alias, Class, Paired,
Bonded, Connected, RSSI, TxPower, UUIDs, ManufacturerData {int: bytes},
ServiceData {uuid: bytes}, AdvertisingData {int: bytes}, AdvertisingFlags,
Appearance, ServicesResolved, Blocked).

Backend methods the core calls (callbacks ok(...) / err(name, message)):

    adapter() -> {"Address", "Name", "Class", "Powered", "AdvInstances"}
    devices() -> {address: props}
    discover(transport or None, ok, err)    None stops; a transport (re)starts
    connect(addr, ok, err) / disconnect(addr, ok, err)
    pair(addr, ok, err) / cancel_pair(addr) / remove(addr, ok, err)
    gatt_db(addr) -> [(kind, handle, start, end, uuid, properties)]
    attribute(addr, handle) -> ("char"|"desc"|"service", uuid, parent char handle or None) or None
    read(addr, handle, offset, ok(value), err)
    write(addr, handle, value, wtype, offset, ok, err)
    notify(addr, char_handle, on, ok, err)
    mtu(addr) -> int
    after(seconds, fn) -> handle;  cancel(handle)

and calls back: device_changed(addr, props_changed, added), device_removed(addr),
adapter_changed(props), value_changed(addr, char_handle, value).
"""
import re
import uuid

from . import proto as P

CONNECT_SECONDS = 35
DISCOVER_SECONDS = 30
CCCD = "00002902-0000-1000-8000-00805f9b34fb"
AD_FIELDS = ("RSSI", "ManufacturerData", "ServiceData", "AdvertisingData",
             "AdvertisingFlags", "Name", "Alias", "TxPower", "UUIDs", "Appearance")
PAIR = "pair"   # the hold token of a Pair() in flight


class Hold:
    """A device the bridge connected (or is connecting) for Android."""

    def __init__(self, made):
        self.made = made          # we called Connect() (or Pair() on a disconnected device)
        self.tokens = set()       # link ids, and PAIR
        self.state = "connecting"
        self.timer = None


class Link:
    def __init__(self, link_id, client, addr):
        self.id = link_id
        self.client = client
        self.addr = addr
        self.notify = set()       # characteristic value handles this link enabled
        self.cccd = {}            # descriptor handle -> last value written


class Bridge:
    def __init__(self, backend, store, log=None):
        self.be = backend
        self.store = store
        self.log = log or (lambda *a: None)
        self.devs = {}
        self.clients = {}         # client -> {"enabled": bool, "scans": set(transports)}
        self.links = {}
        self.holds = {}
        self.next_link = 1
        self.frozen = False
        self.discovering = None   # the transport BlueZ discovery runs with for us, or None
        self.pairing = {}         # addr -> client
        self.resolving = {}       # addr -> [callbacks waiting for ServicesResolved]
        self.notifying = {}       # (addr, char handle) -> count of links
        self.le_seen = set()      # addresses reported by an LE scan

    # ---- state from the backend -------------------------------------------

    def start(self):
        self.devs = {a: dict(p) for a, p in self.be.devices().items()}

    def visible(self, addr):
        d = self.devs.get(addr)
        if d is None or d.get("Blocked"):
            return False
        return not (d.get("Connected") and addr not in self.holds)

    def bonded(self, d):
        return bool(d.get("Bonded", d.get("Paired")))

    def dev_tuple(self, addr):
        d = self.devs[addr]
        random = d.get("AddressType") == "random"
        if random:
            dtype = P.DEV_BLE
        elif "Class" in d:
            dtype = P.DEV_DUAL if addr in self.le_seen else P.DEV_BREDR
        else:
            dtype = P.DEV_BLE
        if addr in self.pairing:
            bond = P.BOND_BONDING
        else:
            bond = P.BOND_BONDED if self.bonded(d) else P.BOND_NONE
        name = d.get("Name") or (d.get("Alias") if "Name" in d else "") or ""
        return (addr, P.ADDR_RANDOM if random else P.ADDR_PUBLIC, name,
                int(d.get("Class", 0)), dtype, bond,
                bool(d.get("Connected")) and addr in self.holds,
                [u for u in d.get("UUIDs", [])])

    def device_changed(self, addr, changed, added=False):
        old = self.devs.get(addr, {})
        was_visible = addr in self.devs and self.visible(addr)
        was_connected = bool(old.get("Connected"))
        was_bonded = self.bonded(old) if old else False
        d = self.devs.setdefault(addr, {"Address": addr})
        d.update(changed)

        if "Connected" in changed and bool(changed["Connected"]) != was_connected:
            self._connected_changed(addr, bool(changed["Connected"]))
        if "ServicesResolved" in changed and changed["ServicesResolved"]:
            for cb in self.resolving.pop(addr, []):
                cb(True)
        if (("Paired" in changed or "Bonded" in changed) and self.bonded(d) != was_bonded
                and addr not in self.pairing and self.visible(addr)):
            self._event_all("EV_BOND_STATE", [addr, P.BOND_BONDED if self.bonded(d) else P.BOND_NONE, P.ST_OK])
        if was_visible and not self.visible(addr):
            self.log("hidden from Android: Linux uses", addr)
        if self.visible(addr) and (added or any(k in changed for k in AD_FIELDS)):
            self._report_found(addr)

    def device_removed(self, addr):
        if addr in self.holds:
            self._drop_hold(addr, P.ST_FAIL)
        self.devs.pop(addr, None)
        self.le_seen.discard(addr)

    def adapter_changed(self, changed):
        if "Powered" in changed or "Name" in changed or "Alias" in changed:
            a = self.be.adapter()
            self._event_all("EV_ADAPTER", [bool(a.get("Powered")), a.get("Name", "")])
        if "Powered" in changed and not changed["Powered"]:
            self.discovering = None
            for addr in list(self.holds):
                self._drop_hold(addr, P.ST_NOT_READY)

    def value_changed(self, addr, handle, value):
        for link in list(self.links.values()):
            if link.addr == addr and handle in link.notify:
                self._event(link.client, "EV_GATT_VALUE", [link.id, handle, value], droppable=False)

    def set_frozen(self, frozen):
        """Android's container froze (or thawed). Frozen: no discovery, no links."""
        if frozen == self.frozen:
            return
        self.frozen = frozen
        self.log("container", "frozen: releasing Android's devices" if frozen else "thawed")
        if frozen:
            for addr in list(self.holds):
                self._drop_hold(addr, P.ST_NOT_READY, keep_pair=True)
        self._update_discovery()

    # ---- clients ----------------------------------------------------------

    def attach(self, client):
        self.clients[client] = {"hello": False, "enabled": False, "scans": set()}

    def detach(self, client):
        if client not in self.clients:
            return
        del self.clients[client]
        for link in [l for l in self.links.values() if l.client is client]:
            self._close_link(link)
        for addr, c in list(self.pairing.items()):
            if c is client:
                self.pairing[addr] = None   # Pair() goes on: the person may be answering
        self._update_discovery()

    def _event(self, client, op, values, droppable=False):
        try:
            client.send(P.message(op, 0, values), droppable=droppable)
        except P.ProtoError as e:
            self.log("event", op, "not sent:", e)

    def _event_all(self, op, values, droppable=False):
        for c, st in self.clients.items():
            if st["enabled"]:
                self._event(c, op, values, droppable)

    def handle(self, client, packet):
        try:
            op, msg_id, args = P.decode_request(packet)
        except P.ProtoError as e:
            self.log("bad packet:", e)
            client.close()
            return
        done = [False]

        def answer(status, values=()):
            if done[0]:
                return
            done[0] = True
            if client in self.clients:
                client.send(P.reply(op, msg_id, status, values))
        fn = getattr(self, "op_" + op.lower(), None)
        if fn is None:
            answer(P.ST_UNSUPPORTED)
            return
        if client not in self.clients:
            self.attach(client)
        if op != "HELLO" and not self.clients[client]["hello"]:
            answer(P.ST_NOT_READY)
            return
        try:
            fn(client, answer, *args)
        except P.ProtoError as e:
            self.log(op, "bad arguments:", e)
            answer(P.ST_INVALID)

    # ---- ops: adapter -----------------------------------------------------

    def op_hello(self, client, answer, version, name):
        if version != P.VERSION:
            answer(P.ST_UNSUPPORTED)
            return
        self.clients[client]["hello"] = True
        a = self.be.adapter()
        answer(P.ST_OK, [P.VERSION, a.get("Address", "00:00:00:00:00:00"), a.get("Name", ""),
                         int(a.get("Class", 0)), bool(a.get("Powered")), int(a.get("AdvInstances", 0))])

    def op_enable(self, client, answer, on):
        st = self.clients[client]
        st["enabled"] = on
        if not on:
            st["scans"].clear()
            for link in [l for l in self.links.values() if l.client is client]:
                self._close_link(link)
            self._update_discovery()
        answer(P.ST_OK)

    def _ready(self, client, answer):
        if not self.clients[client]["enabled"] or self.frozen or not self.be.adapter().get("Powered"):
            answer(P.ST_NOT_READY)
            return False
        return True

    def op_bonded_list(self, client, answer):
        answer(P.ST_OK, [[self.dev_tuple(a) for a, d in sorted(self.devs.items())
                          if self.bonded(d) and self.visible(a)]])

    def op_device_get(self, client, answer, addr):
        if addr not in self.devs:
            answer(P.ST_NOT_FOUND)
        elif not self.visible(addr):
            answer(P.ST_BUSY)
        else:
            answer(P.ST_OK, [self.dev_tuple(addr)])

    def op_rssi(self, client, answer, addr):
        d = self.devs.get(addr)
        if d is None or not self.visible(addr):
            answer(P.ST_NOT_FOUND if d is None else P.ST_BUSY)
        elif "RSSI" not in d:
            answer(P.ST_NOT_READY)
        else:
            answer(P.ST_OK, [int(d["RSSI"])])

    # ---- ops: scanning ----------------------------------------------------

    def op_scan_start(self, client, answer, transport):
        if transport not in (P.T_AUTO, P.T_BREDR, P.T_LE):
            answer(P.ST_INVALID)
            return
        if not self._ready(client, answer):
            return
        self.clients[client]["scans"].add(transport)
        self._update_discovery(answer)

    def op_scan_stop(self, client, answer, transport):
        self.clients[client]["scans"].discard(transport)
        self._update_discovery(answer)

    def _wanted_transport(self):
        if self.frozen:
            return None
        ts = set()
        for st in self.clients.values():
            if st["enabled"]:
                ts |= st["scans"]
        if not ts:
            return None
        if ts == {P.T_LE}:
            return P.T_LE
        if ts == {P.T_BREDR}:
            return P.T_BREDR
        return P.T_AUTO

    def _update_discovery(self, answer=None):
        want = self._wanted_transport()
        if want == self.discovering:
            if answer:
                answer(P.ST_OK)
            return
        prev, self.discovering = self.discovering, want

        def ok():
            if answer:
                answer(P.ST_OK)
            if want is not None:   # devices already known show at once
                for addr in list(self.devs):
                    if self.visible(addr) and "RSSI" in self.devs[addr]:
                        self._report_found(addr)

        def err(name, msg):
            self.log("discovery", want, "failed:", name, msg)
            if self.discovering == want:
                self.discovering = prev if want is not None else None
            if answer:
                answer(P.ST_FAIL)
        self.be.discover(want, ok, err)

    def _report_found(self, addr):
        if self.discovering is None:
            return
        d = self.devs[addr]
        if "RSSI" not in d:      # not heard in this discovery (BlueZ drops RSSI when it ends)
            return
        if self.discovering in (P.T_LE, P.T_AUTO) and (
                d.get("AddressType") == "random" or "AdvertisingFlags" in d
                or "ManufacturerData" in d or "ServiceData" in d or "AdvertisingData" in d):
            self.le_seen.add(addr)
        tx = int(d["TxPower"]) if "TxPower" in d else 127
        values = [self.dev_tuple(addr), int(d["RSSI"]), tx, build_ad(d)]
        for c, st in self.clients.items():
            if st["enabled"] and st["scans"]:
                self._event(c, "EV_DEVICE_FOUND", values, droppable=True)

    # ---- ops: GATT client -------------------------------------------------

    def op_gatt_open(self, client, answer, addr, addr_type, transport):
        if not self._ready(client, answer):
            return
        if addr not in self.devs:
            answer(P.ST_NOT_FOUND)
            return
        if not self.visible(addr):
            answer(P.ST_BUSY)
            return
        link = Link(self.next_link, client, addr)
        self.next_link += 1
        self.links[link.id] = link
        answer(P.ST_OK, [link.id])
        hold = self.holds.get(addr)
        if hold is None:
            hold = self.holds[addr] = Hold(made=not self.devs[addr].get("Connected"))
            hold.tokens.add(link.id)
            self._connect(addr, hold)
        else:
            hold.tokens.add(link.id)
            if hold.state == "connected":
                self._event(client, "EV_LINK", [link.id, P.ST_OK, True, self.be.mtu(addr)])

    def _connect(self, addr, hold):
        def ok():
            if self.holds.get(addr) is not hold:
                return
            self._connected(addr, hold)

        def err(name, msg):
            if self.holds.get(addr) is not hold:
                return
            self.log("connect", addr, "failed:", name, msg)
            self._drop_hold(addr, P.ST_TIMEOUT if "Timeout" in name or "timeout" in msg.lower() else P.ST_FAIL)

        def timeout():
            hold.timer = None
            if self.holds.get(addr) is hold and hold.state == "connecting":
                self.log("connect", addr, "timed out")
                self._drop_hold(addr, P.ST_TIMEOUT)
        hold.timer = self.be.after(CONNECT_SECONDS, timeout)
        self.log("connect", addr, "for Android")
        self.be.connect(addr, ok, err)

    def _connected(self, addr, hold):
        if hold.timer is not None:
            self.be.cancel(hold.timer)
            hold.timer = None
        if hold.state == "connected":
            return
        hold.state = "connected"
        mtu = self.be.mtu(addr)
        for token in hold.tokens:
            link = self.links.get(token)
            if link:
                self._event(link.client, "EV_LINK", [link.id, P.ST_OK, True, mtu])
        self._event_all("EV_ACL", [addr, True, P.T_LE if addr in self.le_seen or "Class" not in self.devs[addr] else P.T_BREDR])

    def _connected_changed(self, addr, connected):
        hold = self.holds.get(addr)
        if hold is None:
            return
        if connected and hold.state == "connecting":
            # Connect() or Pair() got there: links hear now, Connect()'s return is a no-op
            self._connected(addr, hold)
            return
        if not connected and hold.state == "connected":
            self.log("disconnected:", addr)
            self._drop_hold(addr, P.ST_OK, disconnect=False)

    def _drop_hold(self, addr, status, disconnect=True, keep_pair=False):
        """End every Android link to addr; disconnect if we made the connection."""
        hold = self.holds.get(addr)
        if hold is None:
            return
        if keep_pair and PAIR in hold.tokens:
            for token in [t for t in hold.tokens if t != PAIR]:
                self._end_link(token, status)
                hold.tokens.discard(token)
            return
        del self.holds[addr]
        if hold.timer is not None:
            self.be.cancel(hold.timer)
        for token in list(hold.tokens):
            self._end_link(token, status)
        if disconnect and hold.made:
            self.be.disconnect(addr, lambda: None, lambda n, m: self.log("disconnect", addr, n, m))
        self._event_all("EV_ACL", [addr, False, P.T_AUTO])

    def _end_link(self, token, status):
        link = self.links.pop(token, None) if isinstance(token, int) else None
        if link is None:
            return
        self._unnotify(link)
        self._event(link.client, "EV_LINK", [link.id, status, False, 0])

    def _close_link(self, link):
        self.links.pop(link.id, None)
        self._unnotify(link)
        self._release(link.addr, link.id)

    def _release(self, addr, token):
        hold = self.holds.get(addr)
        if hold is None:
            return
        hold.tokens.discard(token)
        if not hold.tokens:
            self._drop_hold(addr, P.ST_OK)

    def _unnotify(self, link):
        for h in list(link.notify):
            self._notify_ref(link.addr, h, -1, lambda: None, lambda n, m: None)
        link.notify.clear()

    def _notify_ref(self, addr, handle, delta, ok, err):
        key = (addr, handle)
        n = self.notifying.get(key, 0)
        new = max(0, n + delta)
        if new:
            self.notifying[key] = new
        else:
            self.notifying.pop(key, None)
        if (n == 0) != (new == 0) and self.devs.get(addr, {}).get("Connected"):
            self.be.notify(addr, handle, new > 0, ok, err)
        else:
            ok()

    def op_gatt_close(self, client, answer, link_id):
        link = self.links.get(link_id)
        if link is None or link.client is not client:
            answer(P.ST_NOT_FOUND)
            return
        self._close_link(link)
        answer(P.ST_OK)

    def _link(self, client, answer, link_id):
        link = self.links.get(link_id)
        if link is None or link.client is not client:
            answer(P.ST_NOT_FOUND)
            return None
        hold = self.holds.get(link.addr)
        if hold is None or hold.state != "connected":
            answer(P.ST_NOT_READY)
            return None
        return link

    def op_gatt_discover(self, client, answer, link_id):
        link = self._link(client, answer, link_id)
        if link is None:
            return
        addr = link.addr

        def resolved(ok):
            if not ok:
                answer(P.ST_TIMEOUT)
            elif self.links.get(link_id) is not link:
                answer(P.ST_NOT_READY)
            else:
                answer(P.ST_OK, [self.be.gatt_db(addr)])
        if self.devs[addr].get("ServicesResolved"):
            resolved(True)
            return
        waiters = self.resolving.setdefault(addr, [])
        waiters.append(resolved)

        def timeout():
            if resolved in self.resolving.get(addr, []):
                self.resolving[addr].remove(resolved)
                resolved(False)
        self.be.after(DISCOVER_SECONDS, timeout)

    def _att_answer(self, answer):
        def err(name, msg):
            code = att_code(name, msg)
            if code is not None:
                answer(P.ST_ATT, [code])
            elif "InProgress" in name:
                answer(P.ST_IN_PROGRESS)
            elif "NotSupported" in name:
                answer(P.ST_UNSUPPORTED)
            else:
                answer(P.ST_FAIL)
        return err

    def op_gatt_read(self, client, answer, link_id, handle, offset):
        link = self._link(client, answer, link_id)
        if link is None:
            return
        attr = self.be.attribute(link.addr, handle)
        if attr is None:
            answer(P.ST_ATT, [0x01])           # invalid handle
            return
        if attr[0] == "desc" and P.full_uuid(attr[1]) == CCCD:
            answer(P.ST_OK, [link.cccd.get(handle, b"\x00\x00")[offset:]])
            return
        self.be.read(link.addr, handle, offset, lambda v: answer(P.ST_OK, [bytes(v)]), self._att_answer(answer))

    def op_gatt_write(self, client, answer, link_id, handle, wtype, offset, value):
        link = self._link(client, answer, link_id)
        if link is None:
            return
        attr = self.be.attribute(link.addr, handle)
        if attr is None:
            answer(P.ST_ATT, [0x01])
            return
        if wtype == P.W_PREPARE:
            answer(P.ST_UNSUPPORTED)           # reliable writes: not yet
            return
        if attr[0] == "desc" and P.full_uuid(attr[1]) == CCCD:
            self._write_cccd(link, handle, attr[2], value, answer)
            return
        if wtype not in (P.W_NO_RSP, P.W_DEFAULT):
            answer(P.ST_INVALID)
            return
        self.be.write(link.addr, handle, bytes(value), wtype, offset,
                      lambda: answer(P.ST_OK), self._att_answer(answer))

    def _write_cccd(self, link, handle, char_handle, value, answer):
        if len(value) != 2 or char_handle is None:
            answer(P.ST_ATT, [0x0d])           # invalid attribute value length
            return
        on = bool(value[0] & 0x03)
        was = char_handle in link.notify
        link.cccd[handle] = bytes(value)
        if on == was:
            answer(P.ST_OK)
            return
        if on:
            link.notify.add(char_handle)
        else:
            link.notify.discard(char_handle)

        def err(name, msg):
            if on:
                link.notify.discard(char_handle)
            self._att_answer(answer)(name, msg)
        self._notify_ref(link.addr, char_handle, 1 if on else -1, lambda: answer(P.ST_OK), err)

    def op_gatt_mtu(self, client, answer, link_id):
        link = self._link(client, answer, link_id)
        if link is not None:
            answer(P.ST_OK, [self.be.mtu(link.addr)])

    # ---- ops: bonds -------------------------------------------------------

    def op_bond_create(self, client, answer, addr, transport):
        if not self._ready(client, answer):
            return
        d = self.devs.get(addr)
        if d is None:
            answer(P.ST_NOT_FOUND)
            return
        if not self.visible(addr):
            answer(P.ST_BUSY)
            return
        if addr in self.pairing:
            answer(P.ST_IN_PROGRESS)
            return
        answer(P.ST_OK)
        if self.bonded(d):
            self._event(client, "EV_BOND_STATE", [addr, P.BOND_BONDED, P.ST_OK])
            return
        self.pairing[addr] = client
        hold = self.holds.get(addr)
        if hold is None:
            hold = self.holds[addr] = Hold(made=not d.get("Connected"))
            if d.get("Connected"):
                hold.state = "connected"
        hold.tokens.add(PAIR)
        self._event(client, "EV_BOND_STATE", [addr, P.BOND_BONDING, P.ST_OK])
        self.store.pairing_hint(addr)
        self.log("pair", addr, "for Android (through BlueZ's agent)")

        def finish(state, status):
            self.store.pairing_hint(None)
            owner = self.pairing.pop(addr, None)
            if state == P.BOND_BONDED:
                self.store.add_android_bond(addr)
            if owner in self.clients:
                self._event(owner, "EV_BOND_STATE", [addr, state, status])
            self._release(addr, PAIR)

        def err(name, msg):
            self.log("pair", addr, "failed:", name, msg)
            if name.endswith("AlreadyExists"):
                finish(P.BOND_BONDED, P.ST_OK)
            else:
                finish(P.BOND_NONE, pair_status(name))
        self.be.pair(addr, lambda: finish(P.BOND_BONDED, P.ST_OK), err)

    def op_bond_cancel(self, client, answer, addr):
        if addr not in self.pairing:
            answer(P.ST_NOT_FOUND)
            return
        self.be.cancel_pair(addr)
        answer(P.ST_OK)

    def op_bond_remove(self, client, answer, addr):
        d = self.devs.get(addr)
        if d is None or not self.bonded(d):
            answer(P.ST_NOT_FOUND)
            return
        if not self.store.is_android_bond(addr) or not self.visible(addr):
            self.log("remove bond", addr, "refused: not made by Android")
            answer(P.ST_NOT_PERMITTED)
            return

        def ok():
            self.store.forget_android_bond(addr)
            answer(P.ST_OK)
            self._event_all("EV_BOND_STATE", [addr, P.BOND_NONE, P.ST_OK])
        self.be.remove(addr, ok, lambda n, m: answer(P.ST_FAIL))


def pair_status(name):
    for key, st in (("AuthenticationCanceled", P.ST_CANCELLED), ("AuthenticationRejected", P.ST_AUTH_FAILED),
                    ("AuthenticationFailed", P.ST_AUTH_FAILED), ("AuthenticationTimeout", P.ST_TIMEOUT),
                    ("ConnectionAttemptFailed", P.ST_FAIL), ("InProgress", P.ST_IN_PROGRESS)):
        if name.endswith(key):
            return st
    return P.ST_AUTH_FAILED


_ATT_RE = re.compile(r"ATT error: 0x([0-9a-fA-F]{2})")
_ATT_BY_NAME = {"NotPermitted": 0x03, "NotAuthorized": 0x08, "InvalidOffset": 0x07,
                "InvalidValueLength": 0x0d}


def att_code(name, msg):
    """An ATT error code from a BlueZ GATT error, or None."""
    m = _ATT_RE.search(msg or "")
    if m:
        return int(m.group(1), 16)
    for key, code in _ATT_BY_NAME.items():
        if name.endswith(key):
            if key == "NotPermitted" and "ead" in (msg or ""):
                return 0x02           # read not permitted
            if key == "NotPermitted" and "uthentication" in (msg or ""):
                return 0x05
            return code
    return None


def _ad(t, data):
    data = bytes(data)
    if len(data) > 254:
        return b""
    return bytes([len(data) + 1, t]) + data


def build_ad(d):
    """Advertising data (AD structures) rebuilt from Device1's properties.

    BlueZ parses the advertisement into properties and keeps the types it
    doesn't parse in AdvertisingData; this puts them back together for
    Android's ScanRecord. Order: flags, UUIDs, name, tx power, appearance,
    service data, manufacturer data, the rest."""
    out = bytearray()
    raw = {int(k): bytes(v) for k, v in (d.get("AdvertisingData") or {}).items()}
    if d.get("AdvertisingFlags"):
        out += _ad(0x01, d["AdvertisingFlags"])
        raw.pop(0x01, None)
    u16, u128 = [], []
    for u in d.get("UUIDs", []):
        s = P.short_uuid(u)
        if s:
            u16.append(int(s, 16).to_bytes(2, "little"))
        else:
            u128.append(uuid.UUID(P.full_uuid(u)).bytes[::-1])
    if u16 and 0x02 not in raw and 0x03 not in raw:
        out += _ad(0x03, b"".join(u16))
    if u128 and 0x06 not in raw and 0x07 not in raw:
        out += _ad(0x07, b"".join(u128))
    if d.get("Name") and 0x08 not in raw and 0x09 not in raw:
        out += _ad(0x09, d["Name"].encode("utf-8")[:29])
    if "TxPower" in d and 0x0a not in raw:
        out += _ad(0x0a, (int(d["TxPower"]) & 0xff).to_bytes(1, "little"))
    if "Appearance" in d and 0x19 not in raw:
        out += _ad(0x19, int(d["Appearance"]).to_bytes(2, "little"))
    for u, v in (d.get("ServiceData") or {}).items():
        s = P.short_uuid(u)
        if s:
            out += _ad(0x16, int(s, 16).to_bytes(2, "little") + bytes(v))
        else:
            out += _ad(0x21, uuid.UUID(P.full_uuid(u)).bytes[::-1] + bytes(v))
    for cid, v in (d.get("ManufacturerData") or {}).items():
        out += _ad(0xff, int(cid).to_bytes(2, "little") + bytes(v))
    for t, v in sorted(raw.items()):
        out += _ad(t, v)
    return bytes(out)
