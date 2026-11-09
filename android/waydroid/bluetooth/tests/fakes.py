"""A fake BlueZ backend, store and client for ztos-btbridge's tests."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from ztbt import proto as P  # noqa: E402
from ztbt.core import Bridge  # noqa: E402

AIRPODS = "AA:00:00:00:00:01"     # bonded, connected by Linux
SENSOR = "C0:00:00:00:00:02"      # LE, random, not bonded
LOCK = "D0:00:00:00:00:03"        # LE, bonded, not connected
SPEAKER = "E0:00:00:00:00:04"     # Classic, bonded, not connected
PRINTER = "E8:00:00:00:00:05"     # Classic SPP printer, not bonded yet
SPP = "00001101-0000-1000-8000-00805f9b34fb"

HR = "0000180d-0000-1000-8000-00805f9b34fb"
HRM = "00002a37-0000-1000-8000-00805f9b34fb"
CCCD = "00002902-0000-1000-8000-00805f9b34fb"


class FakeBackend:
    def __init__(self):
        self.calls = []
        self.pending = []      # (name, args, ok, err): answered by the test
        self.timers = []
        self.relays = []
        self.peers = []          # connected LE centrals (server_peers)
        self.powered = True
        self.devs = {
            AIRPODS: {"Address": AIRPODS, "AddressType": "public", "Name": "AirPods", "Alias": "AirPods",
                      "Class": 0x240418, "Paired": True, "Bonded": True, "Connected": True,
                      "UUIDs": ["0000110b-0000-1000-8000-00805f9b34fb"]},
            SENSOR: {"Address": SENSOR, "AddressType": "random", "Alias": "C0-00", "Paired": False,
                     "Connected": False, "UUIDs": [HR]},
            LOCK: {"Address": LOCK, "AddressType": "public", "Name": "Lock", "Alias": "Lock", "Paired": True,
                   "Bonded": True, "Connected": False, "UUIDs": []},
            SPEAKER: {"Address": SPEAKER, "AddressType": "public", "Name": "Speaker", "Alias": "Speaker",
                      "Class": 0x240414, "Paired": True, "Bonded": True, "Connected": False, "UUIDs": []},
            PRINTER: {"Address": PRINTER, "AddressType": "public", "Name": "Printer", "Alias": "Printer",
                      "Class": 0x040680, "Paired": False, "Connected": False, "UUIDs": [SPP]},
        }
        # the GATT database of the sensor: service 1..5, characteristic value 3, CCCD 4
        self.db = [(P.DB_PRIMARY, 1, 1, 5, HR, 0), (P.DB_CHARACTERISTIC, 3, 3, 3, HRM, 0x10),
                   (P.DB_DESCRIPTOR, 4, 4, 4, CCCD, 0)]

    def adapter(self):
        return {"Address": "00:B5:D0:3D:D8:44", "Name": "ztos", "Class": 0x6c0110,
                "Powered": self.powered, "AdvInstances": 5}

    def devices(self):
        return {a: dict(p) for a, p in self.devs.items()}

    def _later(self, name, args, ok, err):
        self.calls.append((name,) + tuple(args))
        self.pending.append((name, args, ok, err))

    def discover(self, transport, ok, err):
        self.calls.append(("discover", transport))
        ok()

    def connect(self, addr, ok, err):
        self._later("connect", [addr], ok, err)

    def disconnect(self, addr, ok, err):
        self.calls.append(("disconnect", addr))
        ok()

    def pair(self, addr, ok, err):
        self._later("pair", [addr], ok, err)

    def cancel_pair(self, addr):
        self.calls.append(("cancel_pair", addr))

    def remove(self, addr, ok, err):
        self.calls.append(("remove", addr))
        ok()

    def gatt_db(self, addr):
        return list(self.db)

    def attribute(self, addr, handle):
        return {1: ("service", HR, None), 3: ("char", HRM, None), 4: ("desc", CCCD, 3)}.get(handle)

    def read(self, addr, handle, offset, ok, err):
        self._later("read", [addr, handle, offset], ok, err)

    def write(self, addr, handle, value, wtype, offset, ok, err):
        self._later("write", [addr, handle, value, wtype, offset], ok, err)

    def notify(self, addr, handle, on, ok, err):
        self.calls.append(("notify", addr, handle, on))
        ok()

    def mtu(self, addr):
        return 247

    def serve(self, svc_id, tree, ok, err):
        self._later("serve", [svc_id, tree], ok, err)

    def unserve(self, svc_id):
        self.calls.append(("unserve", svc_id))

    def server_value(self, svc_id, handle, value, indicate):
        self.calls.append(("server_value", svc_id, handle, value, indicate))
        return True

    def server_peers(self):
        return list(self.peers)

    def advertise(self, adv_id, props, ok, err):
        self._later("advertise", [adv_id, props], ok, err)

    def unadvertise(self, adv_id):
        self.calls.append(("unadvertise", adv_id))

    def rfcomm_connect(self, addr, uuid, channel, secure, ok, err):
        self._later("rfcomm", [addr, uuid, channel, secure], ok, err)

    def relay(self, a, b, prefix, on_close):
        r = {"a": a, "b": b, "prefix": prefix, "on_close": on_close, "open": True}
        self.calls.append(("relay", a, b, prefix))
        self.relays.append(r)
        return r

    def unrelay(self, r):
        self.calls.append(("unrelay",))
        if r["open"]:
            r["open"] = False
            os.close(r["a"])
            os.close(r["b"])

    def after(self, seconds, fn):
        self.timers.append((seconds, fn))
        return fn

    def cancel(self, handle):
        self.timers = [t for t in self.timers if t[1] is not handle]

    # test helpers
    def answer(self, name, error=None, value=None, values=None):
        for i, (n, args, ok, err) in enumerate(self.pending):
            if n == name:
                del self.pending[i]
                if error:
                    err(*error)
                elif values is not None:
                    ok(*values)
                elif value is not None:
                    ok(value)
                else:
                    ok()
                return
        raise AssertionError("no pending " + name)

    def called(self, name):
        return [c for c in self.calls if c[0] == name]


class FakeStore:
    def __init__(self):
        self.hint = None
        self.hints = []
        self.android = set()

    def pairing_hint(self, addr):
        self.hint = addr
        self.hints.append(addr)

    def add_android_bond(self, addr):
        self.android.add(addr)

    def is_android_bond(self, addr):
        return addr in self.android

    def forget_android_bond(self, addr):
        self.android.discard(addr)


class FakeClient:
    """The shim's side: sends requests, collects replies and events."""

    def __init__(self, bridge):
        self.bridge = bridge
        self.out = []          # (packet, droppable)
        self.closed = False
        self.next_id = 1

    def send(self, packet, droppable=False):
        self.out.append((packet, droppable))

    def close(self):
        self.closed = True
        self.bridge.detach(self)

    def request(self, op, *values, fds=()):
        msg_id = self.next_id
        self.next_id += 1
        self.bridge.handle(self, P.message(op, msg_id, list(values)), fds)
        return msg_id

    def reply(self, msg_id, op):
        """The reply to msg_id as (status, [values])."""
        for packet, _ in self.out:
            num, _, mid, items = P.decode_raw(packet)
            if num == P.REPLY and mid == msg_id:
                status = items[0][1]
                if status == P.ST_OK:
                    sig = "u" + P.OPS[op][2]
                elif status == P.ST_ATT:
                    sig = "uy"
                else:
                    sig = "u"
                _, _, vals = P.decode_with(sig, packet)
                return vals[0], vals[1:]
        return None

    def events(self, op):
        out = []
        for packet, _ in self.out:
            num = P.HEADER.unpack_from(packet)[0]
            if num == P.OPS[op][0]:
                out.append(P.decode_with(P.OPS[op][1], packet)[2])
        return out

    def droppable(self, op):
        return [d for packet, d in self.out if P.HEADER.unpack_from(packet)[0] == P.OPS[op][0]]


def make():
    be = FakeBackend()
    store = FakeStore()
    br = Bridge(be, store)
    br.start()
    c = FakeClient(br)
    rid = c.request("HELLO", P.VERSION, "test")
    assert c.reply(rid, "HELLO")[0] == P.ST_OK
    rid = c.request("ENABLE", True)
    assert c.reply(rid, "ENABLE")[0] == P.ST_OK
    return be, store, br, c
