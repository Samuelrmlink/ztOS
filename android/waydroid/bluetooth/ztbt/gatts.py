# SPDX-License-Identifier: GPL-2.0-or-later
"""ztos-btbridge: Android apps' GATT servers (stage 2), as BlueZ applications.

Each Android service becomes one BlueZ GATT application (GattManager1
.RegisterApplication of an exported GattService1/Characteristic1/Descriptor1
tree). The handles Android sees are the bridge's; the ATT handles on the air
are BlueZ's.

Requests: BlueZ calls ReadValue/WriteValue on our objects; they go to the
Android app (EV_SERVER_READ/EV_SERVER_WRITE) and its sendResponse comes back
as SERVER_RESPONSE, which answers BlueZ. Write-without-response is answered
at once. Notifications: BlueZ owns every CCCD (it makes one for each
characteristic that notifies or indicates), so the 0x2902 descriptors Android
declares aren't exported; BlueZ's StartNotify/StopNotify become the CCCD
writes Android apps wait for. Android's notify/indicate becomes a Value change.

A remote central becomes a "connection" of Android's servers only once it
touches one of Android's attributes: a device that only uses Linux's
services (the AirPods) never shows up in Android.

Backend (bluez.py or a fake): serve(svc_id, tree, ok, err), unserve(svc_id),
server_value(svc_id, char_handle, value, indicate) -> bool, server_peers() ->
[addresses of connected LE peers]. It calls back server_read,
server_write and server_notify_state.
"""
from . import proto as P

CCCD = "00002902-0000-1000-8000-00805f9b34fb"
RESPONSE_SECONDS = 25           # BlueZ's own D-Bus call times out at 30
ATT_UNLIKELY = 0x0e
ATT_REQUEST_NOT_SUPPORTED = 0x06

# Android's characteristic properties -> BlueZ flags
PROP_FLAGS = ((0x01, "broadcast"), (0x02, "read"), (0x04, "write-without-response"), (0x08, "write"),
              (0x10, "notify"), (0x20, "indicate"), (0x40, "authenticated-signed-writes"),
              (0x80, "extended-properties"))
# Android's attribute permissions -> BlueZ flags (the strictest one wins)
PERM_READ = ((0x04, "encrypt-authenticated-read"), (0x02, "encrypt-read"), (0x01, "read"))
PERM_WRITE = ((0x40, "encrypt-authenticated-write"), (0x20, "encrypt-write"), (0x10, "write"))


class ServerSvc:
    def __init__(self, svc_id, client, elements, handles):
        self.id = svc_id
        self.client = client
        self.elements = elements      # [(kind, uuid, properties, permissions)]
        self.handles = handles        # Android's handle of each element
        self.cccd = {}                # characteristic handle -> its CCCD's handle
        self.kinds = {}               # handle -> kind
        self.served = False


def server_tree(svc):
    """The elements as BlueZ objects: [{kind, handle, uuid, flags, parent}],
    CCCDs left out (BlueZ makes its own). ValueError if the shape is wrong."""
    els = list(zip(svc.elements, svc.handles))
    if not els or els[0][0][0] not in (P.DB_PRIMARY, P.DB_SECONDARY):
        raise ValueError("a service must come first")
    tree, char = [], None
    for (kind, uuid, props, perms), h in els:
        svc.kinds[h] = kind
        if kind in (P.DB_PRIMARY, P.DB_SECONDARY):
            if tree:
                raise ValueError("one service at a time")
            tree.append({"kind": "service", "handle": h, "uuid": uuid, "primary": kind == P.DB_PRIMARY})
        elif kind == P.DB_CHARACTERISTIC:
            flags = [f for bit, f in PROP_FLAGS if props & bit]
            flags += _perm_flags(perms, props)
            char = h
            tree.append({"kind": "char", "handle": h, "uuid": uuid, "flags": sorted(set(flags)),
                         "parent": tree[0]["handle"]})
        elif kind == P.DB_DESCRIPTOR:
            if char is None:
                raise ValueError("a descriptor before any characteristic")
            if P.full_uuid(uuid) == CCCD:
                svc.cccd[char] = h
                continue
            flags = []
            if perms & 0x07:
                flags.append(next(f for bit, f in PERM_READ if perms & bit))
            if perms & 0x70:
                flags.append(next(f for bit, f in PERM_WRITE if perms & bit))
            tree.append({"kind": "desc", "handle": h, "uuid": uuid, "flags": flags or ["read"],
                         "parent": char})
        else:
            raise ValueError("included services: not supported")
    return tree


def _perm_flags(perms, props):
    out = []
    if props & 0x02 and perms & 0x06:          # readable, with encryption required
        out.append(next(f for bit, f in PERM_READ if perms & bit))
    if props & 0x0c and perms & 0x60:
        out.append(next(f for bit, f in PERM_WRITE if perms & bit))
    return out


class GattServerMixin:
    """Bridge's half for GATT servers; Bridge calls _gatts_init()."""

    def _gatts_init(self):
        self.services = {}            # service id -> ServerSvc
        self.next_service = 1
        self.next_handle = 1          # Android-side handles, unique per bridge
        self.server_conns = {}        # address -> conn id
        self.next_server_conn = 1
        self.server_requests = {}     # request id -> (ok, err, timer, svc)
        self.next_request = 1

    # ---- ops ---------------------------------------------------------------

    def op_server_add(self, client, answer, elements):
        if not self._ready(client, answer):
            return
        handles = list(range(self.next_handle, self.next_handle + len(elements)))
        svc = ServerSvc(self.next_service, client, elements, handles)
        try:
            tree = server_tree(svc)
        except ValueError as e:
            self.log("GATT service refused:", e)
            answer(P.ST_INVALID)
            return
        self.next_handle += len(elements)
        self.next_service += 1
        self.services[svc.id] = svc
        self.log("GATT service", svc.id, "for Android:", tree[0]["uuid"], len(tree) - 1, "attributes")

        def ok():
            svc.served = True
            answer(P.ST_OK, [svc.id, handles])

        def err(name, msg):
            self.log("GATT service", svc.id, "refused by BlueZ:", name, msg)
            self.services.pop(svc.id, None)
            answer(P.ST_FAIL)
        svc.tree = tree
        self.be.serve(svc.id, tree, ok, err)

    def op_server_remove(self, client, answer, svc_id):
        svc = self.services.get(svc_id)
        if svc is None or svc.client is not client:
            answer(P.ST_NOT_FOUND)
            return
        self._unserve(svc)
        del self.services[svc_id]
        answer(P.ST_OK)

    def op_server_response(self, client, answer, req_id, att, value):
        req = self.server_requests.pop(req_id, None)
        if req is None or req[3].client is not client:
            answer(P.ST_NOT_FOUND)    # too late (BlueZ gave up), or not Android's
            return
        ok, err, timer, _ = req
        self.be.cancel(timer)
        if att:
            err(att)
        else:
            ok(bytes(value))
        answer(P.ST_OK)

    def op_server_notify(self, client, answer, conn, handle, confirm, value):
        svc = self._svc_of(handle)
        if svc is None or svc.client is not client or not svc.served:
            answer(P.ST_NOT_FOUND)
            return
        # BlueZ notifies every subscribed central: Android names one; there is
        # usually just one
        if self.be.server_value(svc.id, handle, bytes(value), confirm):
            answer(P.ST_OK)
        else:
            answer(P.ST_NOT_READY)

    # ---- from BlueZ ----------------------------------------------------------

    def _svc_of(self, handle):
        for svc in self.services.values():
            if handle in svc.handles:
                return svc
        return None

    def _server_conn(self, svc, addr, mtu=0):
        """The conn id of a central, telling Android when it is new."""
        conn = self.server_conns.get(addr)
        if conn is None:
            conn = self.server_conns[addr] = self.next_server_conn
            self.next_server_conn += 1
            self._event(svc.client, "EV_SERVER_CONN", [conn, addr, True])
            if mtu:
                self._event(svc.client, "EV_SERVER_MTU", [conn, mtu])
        return conn

    def _server_live(self, svc):
        st = self.clients.get(svc.client)
        return st is not None and st["enabled"] and not self.frozen

    def server_read(self, svc_id, handle, addr, offset, mtu, ok, err):
        svc = self.services.get(svc_id)
        if svc is None or not self._server_live(svc):
            err(ATT_UNLIKELY)
            return
        if handle in svc.cccd.values():            # not exported; can't happen
            err(ATT_REQUEST_NOT_SUPPORTED)
            return
        conn = self._server_conn(svc, addr, mtu)
        req = self._server_request(svc, ok, err)
        self._event(svc.client, "EV_SERVER_READ", [conn, req, handle, offset])

    def server_write(self, svc_id, handle, addr, value, offset, need_rsp, mtu, ok, err):
        svc = self.services.get(svc_id)
        if svc is None or not self._server_live(svc):
            err(ATT_UNLIKELY)
            return
        conn = self._server_conn(svc, addr, mtu)
        if not need_rsp:
            ok(b"")                                 # write command: BlueZ waits for nobody
            self._event(svc.client, "EV_SERVER_WRITE", [conn, 0, handle, offset, False, bytes(value)])
            return
        req = self._server_request(svc, lambda _v: ok(b""), err)
        self._event(svc.client, "EV_SERVER_WRITE", [conn, req, handle, offset, True, bytes(value)])

    def server_notify_state(self, svc_id, char_handle, on, indicate=False):
        """BlueZ: the first central subscribed to char_handle, or the last left."""
        svc = self.services.get(svc_id)
        if svc is None or not self._server_live(svc):
            return
        cccd = svc.cccd.get(char_handle)
        if cccd is None:
            return
        peers = self.be.server_peers()
        known = [a for a in peers if a in self.server_conns]
        addr = (known or peers or [None])[0]
        if addr is None:
            return
        conn = self._server_conn(svc, addr)
        value = (b"\x02\x00" if indicate else b"\x01\x00") if on else b"\x00\x00"
        self._event(svc.client, "EV_SERVER_WRITE", [conn, 0, cccd, 0, False, value])

    def _server_request(self, svc, ok, err):
        req = self.next_request
        self.next_request = self.next_request % 0xffffffff + 1

        def timeout():
            if self.server_requests.pop(req, None) is not None:
                self.log("GATT server request", req, "unanswered by Android")
                err(ATT_UNLIKELY)
        self.server_requests[req] = (ok, err, self.be.after(RESPONSE_SECONDS, timeout), svc)
        return req

    def server_peer_gone(self, addr):
        conn = self.server_conns.pop(addr, None)
        if conn is None:
            return
        told = set()
        for svc in self.services.values():
            if svc.client not in told and svc.client in self.clients:
                told.add(svc.client)
                self._event(svc.client, "EV_SERVER_CONN", [conn, addr, False])

    # ---- lifetime ----------------------------------------------------------------

    def _unserve(self, svc):
        for req_id, req in list(self.server_requests.items()):
            if req[3] is svc:
                del self.server_requests[req_id]
                self.be.cancel(req[2])
                req[1](ATT_UNLIKELY)
        if svc.served:
            svc.served = False
            self.be.unserve(svc.id)

    def _servers_pause(self):
        """Frozen or adapter off: BlueZ drops Android's services (peers see
        Service Changed); the definitions stay for _servers_resume."""
        for svc in self.services.values():
            self._unserve(svc)

    def _servers_resume(self):
        for svc in self.services.values():
            if not svc.served and self._server_live(svc):
                self.be.serve(svc.id, svc.tree, lambda s=svc: setattr(s, "served", True),
                              lambda n, m, s=svc: self.log("GATT service", s.id, "not back:", n, m))

    def _servers_forget(self, client):
        for svc_id, svc in list(self.services.items()):
            if svc.client is client:
                self._unserve(svc)
                del self.services[svc_id]
