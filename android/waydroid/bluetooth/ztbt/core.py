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
import os
import re
import time
import uuid

from . import proto as P
from . import rawads as R
from .gatts import GattServerMixin

CONNECT_SECONDS = 35
RELEASE_SECONDS = 10    # how long a device we let go may still show Connected
LINGER_SECONDS = 4      # a device we connected stays up this long after Android's last use
EXPECT_SECONDS = 60     # how long ztos-bt-agent takes an expected address on trust (refreshed)
SILENT_SECONDS = 2      # how often a connected, unused LE device is reported to a scan
SLOW_SECONDS = 5        # a GATT op BlueZ hasn't answered by then is logged
RAW_MAX = 256           # advertisements as received, kept for this many devices
# BlueZ interfaces that mean a Linux profile uses a device (on it or its children)
LINUX_USE = {"org.bluez.Input1", "org.bluez.MediaControl1", "org.bluez.MediaTransport1",
             "org.bluez.MediaPlayer1", "org.bluez.Network1"}
DISCOVER_SECONDS = 30
CCCD = "00002902-0000-1000-8000-00805f9b34fb"
AD_FIELDS = ("RSSI", "ManufacturerData", "ServiceData", "AdvertisingData",
             "AdvertisingFlags", "Name", "Alias", "TxPower", "UUIDs", "Appearance")
PAIR = "pair"   # the hold token of a Pair() in flight
RFCOMM = "rfcomm"   # hold tokens (RFCOMM, stream id)
NO_UUID = "00000000-0000-0000-0000-000000000000"
SEC_ENCRYPT, SEC_AUTH, SEC_AUTH_MITM = 0x01, 0x02, 0x08   # Android's BluetoothSocket SEC_FLAG_*


class Stream:
    """An Android RFCOMM socket: our end of its socketpair, then the relay."""

    def __init__(self, sid, client, addr, fd):
        self.id = sid
        self.client = client
        self.addr = addr
        self.fd = fd              # ours until the relay takes it
        self.relay = None


class Hold:
    """A device the bridge connected (or is connecting) for Android."""

    def __init__(self, made):
        self.made = made          # we called Connect() (or Pair() on a disconnected device)
        self.tokens = set()       # link ids, and PAIR
        self.state = "connecting"
        self.timer = None
        self.linger = None        # the delayed disconnect after Android's last use


class Link:
    def __init__(self, link_id, client, addr):
        self.id = link_id
        self.client = client
        self.addr = addr
        self.notify = set()       # characteristic value handles this link enabled
        self.cccd = {}            # descriptor handle -> last value written
        self.heard = set()        # characteristics a notification came from (for the log)


class Bridge(GattServerMixin):
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
        self.removing = set()     # bonds Android asked BlueZ to remove, under way
        self.resolving = {}       # addr -> [callbacks waiting for ServicesResolved]
        self.notifying = {}       # (addr, char handle) -> count of links
        self.le_seen = set()      # addresses reported by an LE scan
        self.releasing = {}       # addr -> timer: we called Disconnect(), BlueZ not done yet
        self.streams = {}         # stream id -> Stream (RFCOMM)
        self.next_stream = 1
        self.advs = {}            # advertiser id -> Adv (Android's advertising sets)
        self.expected = set()     # addresses ztos-bt-agent may ask about (see _update_expected)
        self.reported = set()     # devices reported to Android in this discovery (for the log)
        self.raw = {}             # addr -> the advertisement as received (raw_ad), newest last
        self.silent_timer = None
        self.expect_timer = None
        self._gatts_init()

    # ---- state from the backend -------------------------------------------

    def start(self):
        self.devs = {a: dict(p) for a, p in self.be.devices().items()}

    def visible(self, addr):
        d = self.devs.get(addr)
        if d is None or d.get("Blocked"):
            return False
        # connected, not ours, and Linux uses it: hidden. A device we just let
        # go still shows Connected until BlueZ is done; it is not Linux's for that.
        return not (d.get("Connected") and addr not in self.holds and addr not in self.releasing
                    and self.linux_uses(addr))

    def linux_uses(self, addr):
        """Is Linux using this connected device, so Android must keep off it?

        A Classic (BR/EDR) device, yes: its profiles are Linux's (audio,
        calls, input; the AirPods, the Fold6). An LE device only when a
        Linux profile sits on it: input (HID over GATT), audio, network.
        An LE device that is connected with none of those (left connected by
        a pairing in Settings, GATT only: RM_2685 on 8 Nov) is not in use,
        and an Android app may connect to it: it shares the link, and the
        bridge never disconnects a link it didn't make."""
        d = self.devs.get(addr, {})
        if "Class" in d and d.get("AddressType") != "random":
            return True
        return bool(set(self.be.device_interfaces(addr)) & LINUX_USE)

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
        # The UUIDs it advertises, as against the services a connection
        # resolved: BlueZ merges both into UUIDs, and a scan record rebuilt
        # from the merged list runs past what Android keeps of one (62 bytes).
        if "UUIDs" in changed and not d.get("Connected") and not d.get("ServicesResolved"):
            d["AdvUUIDs"] = list(changed["UUIDs"])
        if "RSSI" in changed:
            d["LastRSSI"] = changed["RSSI"]     # BlueZ drops RSSI when discovery ends

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
        """BlueZ dropped the device: removed in Settings (bluetoothctl remove)
        or, unbonded, aged out. A bond removal never shows as Paired going
        false - the object just goes - so Android hears of it only here; on
        BOND_NONE Android drops it from its bonded devices itself."""
        if addr in self.holds:
            self._drop_hold(addr, P.ST_FAIL)
        old = self.devs.pop(addr, None)
        self.le_seen.discard(addr)
        self.raw.pop(addr, None)
        if old and self.bonded(old) and addr not in self.pairing and addr not in self.removing:
            self.log("bond gone on Linux:", addr)
            self.store.forget_android_bond(addr)
            self._event_all("EV_BOND_STATE", [addr, P.BOND_NONE, P.ST_OK])

    def adapter_changed(self, changed):
        if "Powered" in changed or "Name" in changed or "Alias" in changed:
            a = self.be.adapter()
            self._event_all("EV_ADAPTER", [bool(a.get("Powered")), a.get("Name", "")])
        if "Powered" in changed and not changed["Powered"]:
            self.discovering = None
            for addr in list(self.holds):
                self._drop_hold(addr, P.ST_NOT_READY)
            self._stop_advs()
            self._servers_pause()
        elif changed.get("Powered"):
            self._servers_resume()

    def value_changed(self, addr, handle, value):
        for link in list(self.links.values()):
            if link.addr == addr and handle in link.notify:
                if handle not in link.heard:
                    link.heard.add(handle)
                    self.log("gatt", addr, "first notification from 0x%04x (%d bytes)" % (handle, len(value)))
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
            self._stop_advs()
            self._servers_pause()
        else:
            self._servers_resume()
        self._update_discovery()

    # ---- clients ----------------------------------------------------------

    def attach(self, client):
        self.clients[client] = {"hello": False, "enabled": False, "scans": set(), "version": 1}

    def detach(self, client):
        if client not in self.clients:
            return
        del self.clients[client]
        self._stop_advs(client, forget=True)
        self._servers_forget(client)
        for link in [l for l in self.links.values() if l.client is client]:
            self._close_link(link)
        self._flush_lingers()               # Android is gone: no reason to wait
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
        for c, st in list(self.clients.items()):   # a send may drop a client
            if st["enabled"]:
                self._event(c, op, values, droppable)

    def handle(self, client, packet, fds=()):
        """One request. fds: the file descriptors that came with it (ours to
        close, or to hand to the op that takes one)."""
        fds = list(fds)

        def close_fds():
            for fd in fds:
                try:
                    os.close(fd)
                except OSError:
                    pass
            fds.clear()
        try:
            op, msg_id, args = P.decode_request(packet)
        except P.ProtoError as e:
            close_fds()
            self.log("bad packet:", e)
            client.close()
            return
        if (op in P.FD_OPS) != (len(fds) == 1) or len(fds) > 1:
            close_fds()
            self.log(op, "came with the wrong number of fds")
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
            close_fds()
            answer(P.ST_UNSUPPORTED)
            return
        if client not in self.clients:
            self.attach(client)
        if op != "HELLO" and not self.clients[client]["hello"]:
            close_fds()
            answer(P.ST_NOT_READY)
            return
        try:
            if fds:
                fd = fds.pop()      # the op owns it now
                fn(client, answer, *args, fd=fd)
            else:
                fn(client, answer, *args)
        except P.ProtoError as e:
            self.log(op, "bad arguments:", e)
            answer(P.ST_INVALID)

    # ---- ops: adapter -----------------------------------------------------

    def op_hello(self, client, answer, version, name):
        if not P.MIN_VERSION <= version <= P.VERSION:
            answer(P.ST_UNSUPPORTED)
            return
        self.clients[client]["hello"] = True
        self.clients[client]["version"] = version
        a = self.be.adapter()
        answer(P.ST_OK, [version, a.get("Address", "00:00:00:00:00:00"), a.get("Name", ""),
                         int(a.get("Class", 0)), bool(a.get("Powered")), int(a.get("AdvInstances", 0))])

    def op_enable(self, client, answer, on):
        st = self.clients[client]
        st["enabled"] = on
        if not on:
            st["scans"].clear()
            for link in [l for l in self.links.values() if l.client is client]:
                self._close_link(link)
            self._flush_lingers()           # Android's Bluetooth is off: let go now
            self._stop_advs(client, forget=True)
            self._servers_forget(client)
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
            self.reported = set()
            self.log("scan for Android:", {P.T_LE: "LE", P.T_BREDR: "Classic", P.T_AUTO: "LE and Classic",
                                           None: "stopped"}[want])
            if want is not None:   # devices already known show at once
                for addr in list(self.devs):
                    if self.visible(addr) and "RSSI" in self.devs[addr]:
                        self._report_found(addr)
            self._silent_ticker(want in (P.T_LE, P.T_AUTO))
            self.be.watch_ads(want in (P.T_LE, P.T_AUTO))

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
        self._send_found(addr, int(d["RSSI"]))

    def _send_found(self, addr, rssi, silent=False):
        d = self.devs[addr]
        tx = int(d["TxPower"]) if "TxPower" in d else 127
        raw = self.raw.get(addr)
        if raw is not None:     # what it sent, as received: what a phone's app would get
            data, etype = raw["record"], raw["type"]
        else:                   # nothing heard raw (yet): rebuilt from BlueZ's properties
            data, etype = build_ad(d), R.ADV_IND
        if addr not in self.reported:
            self.reported.add(addr)
            self.log("scan: reported", addr, repr(d.get("Name", "")),
                     "as received" if raw is not None else "rebuilt",
                     "(connected to Linux, not advertising)" if silent else "")
        dev = self.dev_tuple(addr)
        for c, st in list(self.clients.items()):   # a send may drop a client
            if st["enabled"] and st["scans"]:
                if st.get("version", 1) >= 2:
                    self._event(c, "EV_DEVICE_FOUND2", [dev, rssi, tx, etype, data], droppable=True)
                else:
                    self._event(c, "EV_DEVICE_FOUND", [dev, rssi, tx, data], droppable=True)

    def raw_ad(self, addr, addr_type, rssi, flags, data):
        """An LE advertisement as the controller delivered it (ztos-bt-adwatch):
        its data, then its scan response if the kernel had one to add. A scan
        response on its own goes after the last advertisement heard."""
        old = self.raw.pop(addr, None)
        if flags & R.SCAN_RSP:
            adv = old["adv"] if old else b""
            rec = {"adv": adv, "record": adv + data, "type": R.event_type(flags)}
        else:
            rec = {"adv": data, "record": data, "type": R.event_type(flags)}
        self.raw[addr] = rec
        while len(self.raw) > RAW_MAX:
            del self.raw[next(iter(self.raw))]
        if (self.discovering in (P.T_LE, P.T_AUTO) and addr in self.devs and self.visible(addr)
                and not self.devs[addr].get("Connected")):
            self._send_found(addr, rssi)

    def _connected_unused_le(self, addr):
        """An LE device Linux is connected to but not using: Android may use
        it (visible), but being connected it doesn't advertise, so no scan
        would ever see it - an app that scans for it first finds nothing
        (RM_2685 after a pairing in Settings, 8 Nov)."""
        d = self.devs.get(addr, {})
        le = d.get("AddressType") == "random" or "Class" not in d
        return (le and d.get("Connected") and addr not in self.holds and not d.get("Blocked")
                and not self.linux_uses(addr))

    def _silent_ticker(self, on):
        """While Android scans LE, report the connected, unused LE devices as
        seen, every SILENT_SECONDS (as an advertiser would be), with the
        last signal strength heard."""
        if self.silent_timer is not None:
            self.be.cancel(self.silent_timer)
            self.silent_timer = None
        if not on:
            return
        for addr in list(self.devs):
            if self._connected_unused_le(addr):
                d = self.devs[addr]
                self._send_found(addr, int(d.get("RSSI", d.get("LastRSSI", -60))), silent=True)

        def tick():
            self.silent_timer = None
            if self.discovering in (P.T_LE, P.T_AUTO):
                self._silent_ticker(True)
        self.silent_timer = self.be.after(SILENT_SECONDS, tick)

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
            hold = self.holds[addr] = Hold(made=self._taken_back(addr) or not self.devs[addr].get("Connected"))
            hold.tokens.add(link.id)
            self._update_expected()
            self._connect(addr, hold)
        else:
            self._use(hold)
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
            if name.endswith("AlreadyConnected"):   # taken back while BlueZ was letting go
                self._connected(addr, hold)
                return
            if hold.state == "connected" and self.devs.get(addr, {}).get("Connected"):
                # BlueZ said Connected first, then failed the call (seen on 8 Nov:
                # le-connection-abort-by-local after the link was up): the link is
                # there, and tearing it down broke the app's session
                self.log("connect", addr, "call failed after the link came up (kept):", name, msg)
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
        hold.started = time.monotonic()
        self.be.connect(addr, ok, err)

    def _connected(self, addr, hold):
        if hold.timer is not None:
            self.be.cancel(hold.timer)
            hold.timer = None
        if hold.state == "connected":
            return
        hold.state = "connected"
        if getattr(hold, "started", None):
            self.log("connected", addr, "after %.1f s" % (time.monotonic() - hold.started))
        mtu = self.be.mtu(addr)
        for token in hold.tokens:
            link = self.links.get(token)
            if link:
                self._event(link.client, "EV_LINK", [link.id, P.ST_OK, True, mtu])
        self._event_all("EV_ACL", [addr, True, P.T_LE if addr in self.le_seen or "Class" not in self.devs[addr] else P.T_BREDR])

    def _connected_changed(self, addr, connected):
        if not connected:
            self.server_peer_gone(addr)
            for cb in self.resolving.pop(addr, []):    # a discovery can't finish now
                cb("gone")
        if not connected and addr in self.releasing:
            self.be.cancel(self.releasing.pop(addr))
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
        self._use(hold)
        self._update_expected()
        for token in list(hold.tokens):
            self._end_link(token, status)
        if disconnect and hold.made:
            self._release_timer(addr)
            self.be.disconnect(addr, lambda: None, lambda n, m: self.log("disconnect", addr, n, m))
        self._event_all("EV_ACL", [addr, False, P.T_AUTO])

    def _release_timer(self, addr):
        if addr in self.releasing:
            self.be.cancel(self.releasing[addr])

        def done():
            self.releasing.pop(addr, None)
        self.releasing[addr] = self.be.after(RELEASE_SECONDS, done)

    def _taken_back(self, addr):
        """Android wants a device we are still letting go: it is ours again."""
        if addr in self.releasing:
            self.be.cancel(self.releasing.pop(addr))
            return True
        return False

    def _end_link(self, token, status):
        if isinstance(token, tuple) and token[0] == RFCOMM:
            self._end_stream(token[1])
            return
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
        """Android is done with one use of addr. The last use of a device the
        bridge connected lets it go after LINGER_SECONDS, not at once: apps
        close and open again (nRF Connect around a bond), and a pairing is
        followed by the app's own connection (Nelko's RFCOMM: disconnecting
        right after Pair() tore down its first connect, 8 Nov)."""
        hold = self.holds.get(addr)
        if hold is None:
            return
        hold.tokens.discard(token)
        if hold.tokens:
            return
        if not (hold.made and hold.state == "connected") or self.frozen:
            self._drop_hold(addr, P.ST_OK)
            return

        def linger_over():
            if self.holds.get(addr) is hold and not hold.tokens:
                hold.linger = None
                self._drop_hold(addr, P.ST_OK)
        if hold.linger is not None:
            self.be.cancel(hold.linger)
        hold.linger = self.be.after(LINGER_SECONDS, linger_over)

    def _update_expected(self):
        """Tell ztos-bt-agent which devices this phone is dealing with for
        Android: those the bridge holds (connected for an app) and those in
        its own Pair(). The agent asks the person (its usual sheet) about
        pairing requests from these, as it does while the phone is
        discoverable, and keeps refusing all others. A device that asks for
        pairing when an app touches an encrypted attribute (a security
        request; RM_2685 on 8 Nov) is one the bridge holds. Each address
        carries an expiry (EXPECT_SECONDS), refreshed while it stays."""
        want = set(self.holds) | set(self.pairing)
        if want == self.expected:
            return
        self.expected = want
        self.store.expected(sorted(want), EXPECT_SECONDS)
        if self.expect_timer is not None:
            self.be.cancel(self.expect_timer)
            self.expect_timer = None
        if want:
            def refresh():
                self.expect_timer = None
                self.expected = set()           # forces the rewrite
                self._update_expected()
            self.expect_timer = self.be.after(EXPECT_SECONDS // 2, refresh)

    def _flush_lingers(self):
        for addr, hold in list(self.holds.items()):
            if hold.linger is not None and not hold.tokens:
                self._drop_hold(addr, P.ST_OK)

    def _use(self, hold):
        """Android uses a held device again: no disconnect pending."""
        if hold.linger is not None:
            self.be.cancel(hold.linger)
            hold.linger = None

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

    def _link(self, client, answer, link_id, what="op"):
        link = self.links.get(link_id)
        if link is None or link.client is not client:
            self.log("gatt", what, "on link", link_id, "refused: no such link")
            answer(P.ST_NOT_FOUND)
            return None
        hold = self.holds.get(link.addr)
        if hold is None or hold.state != "connected":
            self.log("gatt", link.addr, what, "refused: not connected (%s)" % (hold.state if hold else "let go"))
            answer(P.ST_NOT_READY)
            return None
        return link

    def op_gatt_discover(self, client, answer, link_id):
        link = self._link(client, answer, link_id)
        if link is None:
            return
        addr = link.addr

        started = time.monotonic()

        def resolved(ok):
            if ok == "gone":
                self.log("gatt", addr, "discovery ended: the link dropped")
                answer(P.ST_NOT_READY)
            elif not ok:
                self.log("gatt", addr, "services not resolved after %d s" % DISCOVER_SECONDS)
                answer(P.ST_TIMEOUT)
            elif self.links.get(link_id) is not link:
                answer(P.ST_NOT_READY)
            else:
                db = self.be.gatt_db(addr)
                self.log("gatt", addr, "database: %d entries (waited %.1f s)" % (len(db), time.monotonic() - started))
                answer(P.ST_OK, [db])
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

    def _logged(self, answer, addr, what):
        """answer(), logging each GATT op's result and how long BlueZ took;
        if BlueZ hasn't answered after SLOW_SECONDS, say so (an app may give up
        before BlueZ's own 30 s timeout)."""
        started = time.monotonic()
        state = {"timer": None}

        def slow():
            state["timer"] = None
            self.log("gatt", addr, what, "still waiting for BlueZ after %d s" % SLOW_SECONDS)
        state["timer"] = self.be.after(SLOW_SECONDS, slow)

        def done(status, values=()):
            if state["timer"] is not None:
                self.be.cancel(state["timer"])
                state["timer"] = None
            ms = (time.monotonic() - started) * 1000
            if status == P.ST_OK:
                extra = " %d bytes" % len(values[0]) if values and isinstance(values[0], bytes) else ""
                self.log("gatt", addr, what, "ok%s in %.0f ms" % (extra, ms))
            elif status == P.ST_ATT:
                self.log("gatt", addr, what, "ATT error 0x%02x in %.0f ms" % (values[0], ms))
            else:
                self.log("gatt", addr, what, "failed (status %d) in %.0f ms" % (status, ms))
            answer(status, values)
        return done

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
        link = self._link(client, answer, link_id, "read 0x%04x" % handle)
        if link is None:
            return
        attr = self.be.attribute(link.addr, handle)
        if attr is None:
            self.log("gatt", link.addr, "read 0x%04x refused: no such handle" % handle)
            answer(P.ST_ATT, [0x01])           # invalid handle
            return
        if attr[0] == "desc" and P.full_uuid(attr[1]) == CCCD:
            self.log("gatt", link.addr, "read CCCD 0x%04x: answered from the bridge" % handle)
            answer(P.ST_OK, [link.cccd.get(handle, b"\x00\x00")[offset:]])
            return
        answer = self._logged(answer, link.addr, "read %s 0x%04x %s" % (attr[0], handle, P.full_uuid(attr[1])))
        self.be.read(link.addr, handle, offset, lambda v: answer(P.ST_OK, [bytes(v)]), self._att_answer(answer))

    def op_gatt_write(self, client, answer, link_id, handle, wtype, offset, value):
        link = self._link(client, answer, link_id, "write 0x%04x" % handle)
        if link is None:
            return
        attr = self.be.attribute(link.addr, handle)
        if attr is None:
            self.log("gatt", link.addr, "write 0x%04x refused: no such handle" % handle)
            answer(P.ST_ATT, [0x01])
            return
        if wtype == P.W_PREPARE:
            self.log("gatt", link.addr, "write 0x%04x refused: a prepared (reliable) write" % handle)
            answer(P.ST_UNSUPPORTED)           # reliable writes: not yet
            return
        if attr[0] == "desc" and P.full_uuid(attr[1]) == CCCD:
            self._write_cccd(link, handle, attr[2], value,
                             self._logged(answer, link.addr, "CCCD 0x%04x (characteristic 0x%04x %s) = %s"
                                          % (handle, attr[2] or 0, self._uuid_of(link.addr, attr[2]),
                                             bytes(value).hex())))
            return
        if wtype not in (P.W_NO_RSP, P.W_DEFAULT):
            answer(P.ST_INVALID)
            return
        answer = self._logged(answer, link.addr, "write %s 0x%04x %s %d bytes %s" % (
            attr[0], handle, P.full_uuid(attr[1]), len(value),
            "without response" if wtype == P.W_NO_RSP else "with response"))
        self.be.write(link.addr, handle, bytes(value), wtype, offset,
                      lambda: answer(P.ST_OK), self._att_answer(answer))

    def _uuid_of(self, addr, handle):
        attr = self.be.attribute(addr, handle) if handle is not None else None
        return P.full_uuid(attr[1]) if attr else "?"

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
            mtu = self.be.mtu(link.addr)
            self.log("gatt", link.addr, "MTU", mtu)
            answer(P.ST_OK, [mtu])

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
            hold = self.holds[addr] = Hold(made=self._taken_back(addr) or not d.get("Connected"))
            if d.get("Connected"):
                hold.state = "connected"
        self._use(hold)
        hold.tokens.add(PAIR)
        self._event(client, "EV_BOND_STATE", [addr, P.BOND_BONDING, P.ST_OK])
        self._update_expected()
        self.log("pair", addr, "for Android (through BlueZ's agent)")

        def finish(state, status):
            owner = self.pairing.pop(addr, None)
            self._update_expected()
            if state == P.BOND_BONDED:
                self.store.add_android_bond(addr)
            if owner in self.clients:
                self._event(owner, "EV_BOND_STATE", [addr, state, status])
            self._release(addr, PAIR)

        def err(name, msg):
            self.log("pair", addr, "failed after %.1f s:" % (time.monotonic() - started), name, msg)
            if name.endswith("AlreadyExists"):
                finish(P.BOND_BONDED, P.ST_OK)
            else:
                finish(P.BOND_NONE, pair_status(name))
        started = time.monotonic()

        def paired():
            self.log("pair", addr, "done after %.1f s" % (time.monotonic() - started))
            finish(P.BOND_BONDED, P.ST_OK)
        self.be.pair(addr, paired, err)

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
        # Settings' "Let Android apps remove Bluetooth pairings" (read now,
        # not cached): on, any bond but one Linux is using; off, none
        if not self.visible(addr) or not self.store.android_may_remove_bonds():
            self.log("remove bond", addr, "refused:",
                     "Linux is using it" if not self.visible(addr) else "switched off in Settings")
            answer(P.ST_NOT_PERMITTED)
            self._event(client, "EV_BOND_STATE", [addr, P.BOND_BONDED, P.ST_NOT_PERMITTED])
            return
        self.log("remove bond", addr, "(made by Android)" if self.store.is_android_bond(addr) else "(made on Linux)")

        def ok():
            self.removing.discard(addr)
            self.store.forget_android_bond(addr)
            if addr in self.devs:     # its InterfacesRemoved may come after this reply
                self.devs[addr].update({"Paired": False, "Bonded": False})
            answer(P.ST_OK)
            self._event_all("EV_BOND_STATE", [addr, P.BOND_NONE, P.ST_OK])

        def err(name, msg):
            self.removing.discard(addr)
            answer(P.ST_FAIL)
        self.removing.add(addr)    # its BOND_NONE goes out with the reply, not from device_removed
        self.be.remove(addr, ok, err)


    # ---- ops: Classic RFCOMM ----------------------------------------------

    def op_rfcomm_connect(self, client, answer, addr, uuid_, channel, flags, fd):
        """Android's BluetoothSocket.connect(). fd is our end of the stream
        Android holds: nothing goes down it until the RFCOMM link is up (then
        the channel and the connect signal, then the data); a failure closes
        it, which Android reports as an IOException."""
        def refuse(status):
            os.close(fd)
            answer(status)
        if not self._ready(client, answer):
            os.close(fd)
            return
        by_uuid = P.full_uuid(uuid_) != NO_UUID
        if not by_uuid and channel <= 0:
            refuse(P.ST_INVALID)
            return
        d = self.devs.get(addr)
        if d is None and by_uuid:
            refuse(P.ST_NOT_FOUND)    # BlueZ's SDP needs a device it knows
            return
        if d is not None and not self.visible(addr):
            refuse(P.ST_BUSY)
            return
        stream = Stream(self.next_stream, client, addr, fd)
        self.next_stream += 1
        self.streams[stream.id] = stream
        answer(P.ST_OK, [stream.id])
        connected = bool(d and d.get("Connected"))
        hold = self.holds.get(addr)
        if hold is None:
            hold = self.holds[addr] = Hold(made=self._taken_back(addr) or not connected)
            if connected:
                hold.state = "connected"
        token = (RFCOMM, stream.id)
        self._use(hold)
        hold.tokens.add(token)
        self._update_expected()     # BlueZ may pair on the way (SSP): its agent may ask
        secure = bool(flags & (SEC_ENCRYPT | SEC_AUTH | SEC_AUTH_MITM))
        self.log("rfcomm", stream.id, addr, "uuid" if by_uuid else "channel",
                 uuid_ if by_uuid else channel, "for Android")

        started = time.monotonic()

        def ok(rfd, chan):
            self.log("rfcomm", stream.id, addr, "up on channel", chan,
                     "after %.1f s" % (time.monotonic() - started))
            if self.streams.get(stream.id) is not stream:
                os.close(rfd)
                return
            h = self.holds.get(addr)
            if h is not None and h.state == "connecting":
                h.state = "connected"
            stream.relay = self.be.relay(stream.fd, rfd, P.connect_signal(addr, chan, 0),
                                         lambda: self._stream_gone(stream.id))
            stream.fd = None

        def err(name, msg):
            self.log("rfcomm", stream.id, addr, "failed after %.1f s:" % (time.monotonic() - started), name, msg)
            self._stream_gone(stream.id)
        self.be.rfcomm_connect(addr, P.full_uuid(uuid_) if by_uuid else None, channel, secure, ok, err)

    def _end_stream(self, sid):
        """Close a stream (its hold token is being dropped by the caller)."""
        stream = self.streams.pop(sid, None)
        if stream is None:
            return
        if stream.relay is not None:
            self.be.unrelay(stream.relay)
        elif stream.fd is not None:
            os.close(stream.fd)
        stream.fd = stream.relay = None

    def _stream_gone(self, sid):
        """Either side closed, or the connect failed: end it, let go of the device."""
        stream = self.streams.get(sid)
        if stream is None:
            return
        self._end_stream(sid)
        self._release(stream.addr, (RFCOMM, sid))


    # ---- ops: LE advertising (stage 2) -------------------------------------

    def _adv(self, client, answer, adv_id):
        adv = self.advs.get(adv_id)
        if adv is None or adv.client is not client:
            answer(P.ST_NOT_FOUND)
            return None
        return adv

    def op_adv_start(self, client, answer, connectable, scannable, min_int, max_int, tx_power, ad, scan_rsp,
                     duration):
        if not self._ready(client, answer):
            return
        if len(self.advs) >= MAX_ANDROID_ADVS:
            answer(P.ST_BUSY)          # leave BlueZ's other instances to Linux
            return
        adv_id = next(i for i in range(1, 256) if i not in self.advs)
        adv = Adv(adv_id, client, connectable, scannable, min_int, max_int, tx_power, ad, scan_rsp, duration)
        self.advs[adv_id] = adv
        try:
            self.log("advertise", adv_id, "for Android:", adv_summary(adv))
        except ValueError:
            pass                       # _advertise refuses it

        def ok(tx):
            answer(P.ST_OK, [adv_id, tx])

        def err(status):
            self.advs.pop(adv_id, None)
            answer(status)
        self._advertise(adv, ok, err)

    def _advertise(self, adv, ok, err):
        try:
            props = adv_props(adv)
        except ValueError as e:
            self.log("advertise", adv.id, "refused:", e)
            err(P.ST_INVALID)
            return

        def done(tx):
            adv.active = True
            ok(adv.tx_power if tx is None else tx)

        def failed(name, msg):
            self.log("advertise", adv.id, "failed:", name, msg)
            err(adv_status(name, msg))
        self.be.advertise(adv.id, props, done, failed)

    def _unadvertise(self, adv):
        if adv.active:
            adv.active = False
            self.be.unadvertise(adv.id)

    def op_adv_set_data(self, client, answer, adv_id, scan_rsp, data):
        adv = self._adv(client, answer, adv_id)
        if adv is None:
            return
        if scan_rsp:
            adv.scan_rsp = data
        else:
            adv.ad = data
        if not adv.active:
            answer(P.ST_OK)
            return
        self._unadvertise(adv)                   # BlueZ takes new data by registering again
        self._advertise(adv, lambda tx: answer(P.ST_OK), answer)

    def op_adv_enable(self, client, answer, adv_id, on, duration):
        adv = self._adv(client, answer, adv_id)
        if adv is None:
            return
        if not on:
            self._unadvertise(adv)
            answer(P.ST_OK)
            return
        if not self._ready(client, answer):
            return
        adv.duration = duration
        if adv.active:
            self._unadvertise(adv)
        self._advertise(adv, lambda tx: answer(P.ST_OK), answer)

    def op_adv_stop(self, client, answer, adv_id):
        adv = self._adv(client, answer, adv_id)
        if adv is None:
            return
        self._unadvertise(adv)
        del self.advs[adv_id]
        answer(P.ST_OK)

    def adv_released(self, adv_id):
        """BlueZ let an advertisement go (its Timeout, or it gave up)."""
        adv = self.advs.get(adv_id)
        if adv is None or not adv.active:
            return
        adv.active = False
        if adv.client in self.clients:
            self._event(adv.client, "EV_ADV_STOPPED", [adv_id, P.ST_TIMEOUT])

    def _stop_advs(self, client=None, forget=False):
        """End Android's advertisements (all, or one client's). forget: the
        client is gone or Android's Bluetooth is off, drop them; otherwise
        (freeze, adapter off) keep the sets and tell Android they stopped."""
        for adv_id, adv in list(self.advs.items()):
            if client is not None and adv.client is not client:
                continue
            was = adv.active
            self._unadvertise(adv)
            if forget:
                del self.advs[adv_id]
            elif was and adv.client in self.clients:
                self._event(adv.client, "EV_ADV_STOPPED", [adv_id, P.ST_NOT_READY])


MAX_ANDROID_ADVS = 4    # of BlueZ's 5 instances on the S9


class Adv:
    def __init__(self, adv_id, client, connectable, scannable, min_int, max_int, tx_power, ad, scan_rsp, duration):
        self.id = adv_id
        self.client = client
        self.connectable = connectable
        self.scannable = scannable
        self.min_int = min_int
        self.max_int = max_int
        self.tx_power = tx_power
        self.ad = ad
        self.scan_rsp = scan_rsp
        self.duration = duration
        self.active = False


def parse_ad(data):
    """AD structures -> [(type, bytes)]; ValueError on a malformed one."""
    out, i = [], 0
    data = bytes(data)
    while i < len(data):
        n = data[i]
        if n == 0:
            break                      # padding
        if i + 1 + n > len(data):
            raise ValueError("AD structure runs past the end")
        out.append((data[i + 1], data[i + 2:i + 1 + n]))
        i += 1 + n
    return out


def _uuid_from(raw):
    if len(raw) == 2:
        return P.full_uuid("%04x" % int.from_bytes(raw, "little"))
    if len(raw) == 4:
        return P.full_uuid("%08x" % int.from_bytes(raw, "little"))
    return str(uuid.UUID(bytes=bytes(raw[::-1])))


def adv_props(adv):
    """An Android advertisement (AD bytes and parameters) as the properties of
    a BlueZ org.bluez.LEAdvertisement1, in plain Python values. BlueZ builds the
    AD itself (and the flags), so the Android bytes are read back into fields:
    service UUIDs, manufacturer and service data, name, appearance, tx power;
    other AD types go into Data. Advertising data and scan response are merged:
    BlueZ decides what goes where."""
    props = {"Type": "peripheral" if adv.connectable else "broadcast",
             "ServiceUUIDs": [], "ManufacturerData": {}, "ServiceData": {}, "Data": {}}
    includes = []
    for t, v in parse_ad(adv.ad) + parse_ad(adv.scan_rsp):
        if t == 0x01:                                  # flags: BlueZ sets them
            # ... and refuses any for a broadcast ("Broadcast cannot set flags")
            if v and v[0] & 0x03 and adv.connectable:
                props["Discoverable"] = True
        elif t in (0x02, 0x03):
            props["ServiceUUIDs"] += [_uuid_from(v[i:i + 2]) for i in range(0, len(v) - 1, 2)]
        elif t in (0x04, 0x05):
            props["ServiceUUIDs"] += [_uuid_from(v[i:i + 4]) for i in range(0, len(v) - 3, 4)]
        elif t in (0x06, 0x07):
            props["ServiceUUIDs"] += [_uuid_from(v[i:i + 16]) for i in range(0, len(v) - 15, 16)]
        elif t in (0x08, 0x09):
            props["LocalName"] = v.decode("utf-8", "replace")
        elif t == 0x0a:
            includes.append("tx-power")
        elif t == 0x19 and len(v) == 2:
            props["Appearance"] = int.from_bytes(v, "little")
        elif t == 0x16 and len(v) >= 2:
            props["ServiceData"][_uuid_from(v[:2])] = bytes(v[2:])
        elif t == 0x20 and len(v) >= 4:
            props["ServiceData"][_uuid_from(v[:4])] = bytes(v[4:])
        elif t == 0x21 and len(v) >= 16:
            props["ServiceData"][_uuid_from(v[:16])] = bytes(v[16:])
        elif t == 0xff and len(v) >= 2:
            props["ManufacturerData"][int.from_bytes(v[:2], "little")] = bytes(v[2:])
        else:
            props["Data"][t] = bytes(v)
    if includes:
        props["Includes"] = includes
    if adv.duration:
        props["Timeout"] = max(1, (adv.duration + 99) // 100)   # 10 ms units -> s
    for k in ("ServiceUUIDs", "ManufacturerData", "ServiceData", "Data"):
        if not props[k]:
            del props[k]
    return props


def adv_summary(adv):
    p = adv_props(adv)
    return ", ".join("%s=%s" % (k, v) for k, v in sorted(p.items()))


def adv_status(name, msg):
    if name.endswith("InvalidLength"):
        return P.ST_INVALID            # too much data for a legacy advertisement
    if name.endswith("NotPermitted") or "Maximum" in msg:
        return P.ST_BUSY               # no instance left
    return P.ST_FAIL


def pair_status(name):
    for key, st in (("AuthenticationCanceled", P.ST_CANCELLED), ("AuthenticationRejected", P.ST_AUTH_FAILED),
                    ("AuthenticationFailed", P.ST_AUTH_FAILED), ("AuthenticationTimeout", P.ST_TIMEOUT),
                    ("ConnectionAttemptFailed", P.ST_FAIL), ("InProgress", P.ST_IN_PROGRESS)):
        if name.endswith(key):
            return st
    return P.ST_AUTH_FAILED


_ATT_RE = re.compile(r"ATT error: 0x([0-9a-fA-F]{2})")


def att_code(name, msg):
    """An ATT error code from a BlueZ GATT error, or None. BlueZ 5.87
    (src/gatt-client.c create_gatt_dbus_error) names some ATT errors and
    passes the others as "Operation failed with ATT error: 0x..":

        NotPermitted "Read not permitted"     0x02
        NotPermitted "Write not permitted"    0x03
        NotPermitted "Not paired"             0x05 (also 0x0f, 0x0c: Android's
                                                    answer to all three is to bond)
        InvalidArguments "Invalid offset"     0x07
        InvalidArguments "Invalid Length"     0x0d
        NotAuthorized                         0x08
        NotSupported                          0x06 (handled by the caller)
    """
    msg = msg or ""
    m = _ATT_RE.search(msg)
    if m:
        return int(m.group(1), 16)
    if name.endswith("NotPermitted"):
        if "paired" in msg or "uthentication" in msg:
            return 0x05
        return 0x02 if "ead" in msg else 0x03
    if name.endswith("InvalidArguments"):
        if "offset" in msg.lower():
            return 0x07
        if "length" in msg.lower():
            return 0x0d
        return None
    if name.endswith("NotAuthorized"):
        return 0x08
    if name.endswith("InvalidOffset"):
        return 0x07
    if name.endswith("InvalidValueLength"):
        return 0x0d
    return None


def _ad(t, data):
    data = bytes(data)
    if len(data) > 254:
        return b""
    return bytes([len(data) + 1, t]) + data


def build_ad(d, _n128=None):
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
    for u in d.get("AdvUUIDs") or d.get("UUIDs", []):
        s = P.short_uuid(u)
        if s:
            u16.append(int(s, 16).to_bytes(2, "little"))
        else:
            u128.append(uuid.UUID(P.full_uuid(u)).bytes[::-1])
    if u16 and 0x02 not in raw and 0x03 not in raw:
        out += _ad(0x03, b"".join(u16))
    if _n128 is None:
        _n128 = len(u128)
    if u128[:_n128] and 0x06 not in raw and 0x07 not in raw:
        out += _ad(0x07, b"".join(u128[:_n128]))
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
    # Android keeps 62 bytes of a legacy advertisement (with its scan
    # response) and cuts the rest, which would cut a UUID list in half and
    # lose all of it: drop 128-bit UUIDs from the end until it fits.
    if len(out) > 62 and _n128 > 0:
        return build_ad(d, _n128 - 1)
    return bytes(out)
