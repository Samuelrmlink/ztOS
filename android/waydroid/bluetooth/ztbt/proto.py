# SPDX-License-Identifier: GPL-2.0-or-later
"""The wire format between the Android shim and ztos-btbridge.

One SOCK_SEQPACKET packet is one message:

    header   op:u16  flags:u16  id:u32           (little endian)
    args     each one type byte, then its value

    y u8   q u16   u u32   i i32   B bool (1 byte)
    a      Bluetooth address, 6 bytes, most significant first ("AA:..." order)
    U      UUID, 16 bytes, big endian
    s      string: u16 length, UTF-8
    b      bytes:  u32 length, bytes
    L      list:   u16 count, then that many values (each with its type byte)
    T      tuple:  u8 count, then that many values

A request (shim -> bridge) has an id > 0. The bridge answers each one with
REPLY carrying the same id; REPLY's first argument is a status (ST_*).
Events (bridge -> shim) have id 0.

OPS is the single table of ops and their argument signatures. Signatures use
the type letters above; "L(...)" is a list whose items are tuples of the
signature in brackets, "LU" a list of UUIDs. The shim's ztbt_proto.h repeats
the numbers; tests/test_proto.py checks the two agree.
"""
import struct
import uuid as _uuid

VERSION = 1
MAX_PACKET = 65536
HEADER = struct.Struct("<HHI")
FLAG_FD = 0x0001        # header flag: the packet carries one fd (SCM_RIGHTS)
FD_OPS = {"RFCOMM_CONNECT"}   # the requests that must carry one

# statuses (REPLY's first argument, and some events')
ST_OK = 0
ST_FAIL = 1
ST_BUSY = 2           # Linux uses the device: hidden from Android
ST_NOT_FOUND = 3
ST_NOT_READY = 4      # Android not enabled, or the adapter is off
ST_UNSUPPORTED = 5
ST_AUTH_FAILED = 6    # pairing refused or failed
ST_NOT_PERMITTED = 7  # policy said no
ST_TIMEOUT = 8
ST_INVALID = 9        # bad arguments
ST_IN_PROGRESS = 10
ST_CANCELLED = 11
ST_ATT = 12           # an ATT error; its code is the reply's second argument

# transports
T_AUTO, T_BREDR, T_LE = 0, 1, 2
# Android's device types (bt_device_type_t)
DEV_BREDR, DEV_BLE, DEV_DUAL = 1, 2, 3
# address types (Android's BLE_ADDR_*)
ADDR_PUBLIC, ADDR_RANDOM = 0, 1
# bond states (bt_bond_state_t)
BOND_NONE, BOND_BONDING, BOND_BONDED = 0, 1, 2
# GATT database entry kinds (btgatt_db_attribute_type_t)
DB_PRIMARY, DB_SECONDARY, DB_INCLUDED, DB_CHARACTERISTIC, DB_DESCRIPTOR = 0, 1, 2, 3, 4
# write types (Android's GATT_WRITE_*)
W_NO_RSP, W_DEFAULT, W_PREPARE = 1, 2, 3

# A device as Android sees it, the tuple T(aysuyyBLU): address, address type,
# name, class of device, device type (DEV_*), bond state, connected, UUIDs.

OPS = {
    # name:            (number, request signature, reply signature after the status)
    "REPLY":           (0x0001, None, None),
    "HELLO":           (0x0002, "us", "uasuBy"),   # version, client -> version, adapter address, name, class, powered, adv instances
    "ENABLE":          (0x0003, "B", ""),          # Android's adapter on/off (never powers BlueZ's off)
    "BONDED_LIST":     (0x0010, "", "L(aysuyyBLU)"),
    "DEVICE_GET":      (0x0011, "a", "T(aysuyyBLU)"),
    "SCAN_START":      (0x0020, "y", ""),          # transport (T_LE for BLE scans, T_BREDR/T_AUTO for discovery)
    "SCAN_STOP":       (0x0021, "y", ""),
    "GATT_OPEN":       (0x0030, "ayy", "u"),       # address, address type, transport -> link
    "GATT_CLOSE":      (0x0031, "u", ""),
    "GATT_DISCOVER":   (0x0032, "u", "L(yqqqUq)"), # link -> (kind, handle, start, end, uuid, properties)
    "GATT_READ":       (0x0033, "uqq", "b"),       # link, handle, offset -> value (status ST_ATT: then "y" code)
    "GATT_WRITE":      (0x0034, "uqyqb", ""),      # link, handle, write type, offset, value
    "GATT_MTU":        (0x0035, "u", "q"),
    "RSSI":            (0x0036, "a", "i"),
    "BOND_CREATE":     (0x0040, "ay", ""),         # address, transport; the result comes as BOND_STATE
    "BOND_CANCEL":     (0x0041, "a", ""),
    "BOND_REMOVE":     (0x0042, "a", ""),
    # Classic RFCOMM. The request carries one fd (FLAG_FD): the bridge's end of
    # a stream socketpair whose other end the shim gave Android as the socket.
    # address, UUID (all zero: none), channel (-1: by SDP), Android's SEC_FLAG_*
    "RFCOMM_CONNECT":  (0x0050, "aUiu", "u"),      # -> stream id; the outcome goes down the stream
    # LE advertising (stage 2). Intervals in 0.625 ms, duration in 10 ms (0: none)
    # connectable, scannable, min interval, max interval, tx power, AD, scan response, duration
    "ADV_START":       (0x0060, "BBuuibbq", "yi"), # -> advertiser id, tx power
    "ADV_SET_DATA":    (0x0061, "yBb", ""),        # id, scan response?, AD
    "ADV_ENABLE":      (0x0062, "yBq", ""),        # id, on, duration
    "ADV_STOP":        (0x0063, "y", ""),
    # GATT server (stage 2). A service: its elements in Android's order, each
    # (type DB_*, UUID, properties, permissions); the reply gives the handles
    # Android uses for them (the bridge's, not BlueZ's ATT handles).
    "SERVER_ADD":      (0x0070, "L(yUqq)", "uLq"), # -> service id, handles
    "SERVER_REMOVE":   (0x0071, "u", ""),          # service id
    "SERVER_RESPONSE": (0x0072, "uyb", ""),        # request id, ATT status (0: ok), value
    "SERVER_NOTIFY":   (0x0073, "uqBb", ""),       # conn, handle, confirm (indication), value
    # events, bridge -> shim
    "EV_ADAPTER":      (0x8001, "Bs", None),       # powered, name
    "EV_DEVICE_FOUND": (0x8010, "T(aysuyyBLU)iib", None),  # device, rssi, tx power (127: unknown), AD structures
    "EV_BOND_STATE":   (0x8012, "ayu", None),      # address, bond state, status
    "EV_ACL":          (0x8013, "aBy", None),      # address, connected, transport
    "EV_SCAN_STOPPED": (0x8020, "yu", None),       # transport, why (status)
    "EV_LINK":         (0x8030, "uuBq", None),     # link, status, connected, mtu
    "EV_GATT_VALUE":   (0x8031, "uqb", None),      # link, handle, value (notification or indication)
    "EV_ADV_STOPPED":  (0x8040, "yu", None),       # advertiser id, why (ST_TIMEOUT: its duration ran out)
    # a remote central as Android's GATT server sees it: conn, address, connected
    "EV_SERVER_CONN":  (0x8050, "uaB", None),
    "EV_SERVER_READ":  (0x8051, "uuqq", None),     # conn, request id, handle, offset
    # conn, request id (0: no response wanted), handle, offset, need response, value
    "EV_SERVER_WRITE": (0x8052, "uuqqBb", None),
    "EV_SERVER_MTU":   (0x8053, "uq", None),       # conn, mtu
}
BY_NUMBER = {v[0]: k for k, v in OPS.items()}
REPLY = OPS["REPLY"][0]


class ProtoError(ValueError):
    pass


def addr_bytes(text):
    parts = text.split(":")
    if len(parts) != 6:
        raise ProtoError("bad address %r" % text)
    return bytes(int(p, 16) for p in parts)


def addr_text(raw):
    return ":".join("%02X" % b for b in raw)


class A(str):
    """An address (string "AA:BB:...") to encode as 'a'."""


class U(str):
    """A UUID (string) to encode as 'U'."""


class Y(int):
    pass


class Q(int):
    pass


class I(int):
    pass


def _enc(v, out):
    if isinstance(v, bool):
        out += b"B" + bytes([1 if v else 0])
    elif isinstance(v, A):
        out += b"a" + addr_bytes(v)
    elif isinstance(v, U):
        out += b"U" + _uuid.UUID(full_uuid(v)).bytes
    elif isinstance(v, Y):
        out += b"y" + struct.pack("<B", v)
    elif isinstance(v, Q):
        out += b"q" + struct.pack("<H", v)
    elif isinstance(v, I):
        out += b"i" + struct.pack("<i", v)
    elif isinstance(v, int):
        out += b"u" + struct.pack("<I", v)
    elif isinstance(v, str):
        raw = v.encode("utf-8")
        if len(raw) > 0xffff:
            raise ProtoError("string too long")
        out += b"s" + struct.pack("<H", len(raw)) + raw
    elif isinstance(v, (bytes, bytearray)):
        out += b"b" + struct.pack("<I", len(v)) + bytes(v)
    elif isinstance(v, tuple):
        out += b"T" + struct.pack("<B", len(v))
        for x in v:
            _enc(x, out)
    elif isinstance(v, list):
        out += b"L" + struct.pack("<H", len(v))
        for x in v:
            _enc(x, out)
    else:
        raise ProtoError("can't encode %r" % (v,))


def _dec(buf, pos):
    if pos >= len(buf):
        raise ProtoError("truncated")
    t = chr(buf[pos]); pos += 1

    def take(n):
        nonlocal pos
        if pos + n > len(buf):
            raise ProtoError("truncated")
        v = buf[pos:pos + n]; pos += n
        return v
    if t == "y":
        return ("y", take(1)[0]), pos
    if t == "q":
        return ("q", struct.unpack("<H", take(2))[0]), pos
    if t == "u":
        return ("u", struct.unpack("<I", take(4))[0]), pos
    if t == "i":
        return ("i", struct.unpack("<i", take(4))[0]), pos
    if t == "B":
        b = take(1)[0]
        if b > 1:
            raise ProtoError("bad bool")
        return ("B", bool(b)), pos
    if t == "a":
        return ("a", addr_text(take(6))), pos
    if t == "U":
        return ("U", str(_uuid.UUID(bytes=bytes(take(16))))), pos
    if t == "s":
        n = struct.unpack("<H", take(2))[0]
        try:
            return ("s", bytes(take(n)).decode("utf-8")), pos
        except UnicodeDecodeError:
            raise ProtoError("bad UTF-8")
    if t == "b":
        n = struct.unpack("<I", take(4))[0]
        return ("b", bytes(take(n))), pos
    if t in "LT":
        n = struct.unpack("<H" if t == "L" else "<B", take(2 if t == "L" else 1))[0]
        items = []
        for _ in range(n):
            item, pos = _dec(buf, pos)
            items.append(item)
        return (t, items), pos
    raise ProtoError("unknown type %r" % t)


def _parse_sig(sig, i=0, stop=None):
    """'ayL(yq)' -> ['a', 'y', ('L', ['y', 'q'])]"""
    out = []
    while i < len(sig):
        c = sig[i]
        if c == stop:
            return out, i + 1
        if c in "LT":
            if i + 1 < len(sig) and sig[i + 1] == "(":
                inner, i = _parse_sig(sig, i + 2, ")")
                out.append((c, inner))
                continue
            if c == "L" and i + 1 < len(sig):
                out.append((c, [sig[i + 1]], True))   # list of plain values
                i += 2
                continue
            raise ProtoError("bad signature %r" % sig)
        out.append(c)
        i += 1
    if stop:
        raise ProtoError("unclosed signature %r" % sig)
    return out, i


def _match(spec, item):
    """Check one decoded item against one signature element; return its plain value."""
    kind, val = item
    if isinstance(spec, str):
        if kind != spec:
            raise ProtoError("expected %s, got %s" % (spec, kind))
        return val
    if spec[0] != kind:
        raise ProtoError("expected %s, got %s" % (spec[0], kind))
    if kind == "T":
        return tuple(_match_all(spec[1], val))
    if len(spec) == 3:     # list of plain values
        return [_match(spec[1][0], x) for x in val]
    return [tuple(_match_all(spec[1], x[1])) if x[0] == "T" else _fail_tuple() for x in val]


def _fail_tuple():
    raise ProtoError("list item is not a tuple")


def _match_all(specs, items):
    if len(specs) != len(items):
        raise ProtoError("expected %d values, got %d" % (len(specs), len(items)))
    return [_match(s, it) for s, it in zip(specs, items)]


def encode(op, msg_id, args, flags=0):
    num = OPS[op][0] if isinstance(op, str) else op
    out = bytearray(HEADER.pack(num, flags, msg_id))
    for a in args:
        _enc(a, out)
    if len(out) > MAX_PACKET:
        raise ProtoError("message too long")
    return bytes(out)


def decode_raw(packet):
    if len(packet) < HEADER.size:
        raise ProtoError("short packet")
    num, flags, msg_id = HEADER.unpack_from(packet)
    items, pos = [], HEADER.size
    while pos < len(packet):
        item, pos = _dec(packet, pos)
        items.append(item)
    return num, flags, msg_id, items


def decode_request(packet):
    """-> (op name, id, [plain args]) checked against OPS; ProtoError otherwise.
    An op in FD_OPS must have FLAG_FD set, any other must not (the caller
    checks that exactly one fd came with it)."""
    num, flags, msg_id, items = decode_raw(packet)
    name = BY_NUMBER.get(num)
    if name is None or num & 0x8000 or name == "REPLY":
        raise ProtoError("unknown request op 0x%04x" % num)
    if msg_id == 0:
        raise ProtoError("request without id")
    if flags & ~FLAG_FD or bool(flags & FLAG_FD) != (name in FD_OPS):
        raise ProtoError("%s: wrong flags 0x%x" % (name, flags))
    specs, _ = _parse_sig(OPS[name][1])
    return name, msg_id, _match_all(specs, items)


def decode_with(sig, packet):
    """Decode any packet against a given signature (tests, the mock shim)."""
    num, _, msg_id, items = decode_raw(packet)
    specs, _ = _parse_sig(sig)
    return BY_NUMBER.get(num), msg_id, _match_all(specs, items)


def typed(sig, values):
    """Wrap plain Python values in the classes that select the wire types of sig."""
    specs, _ = _parse_sig(sig)
    if len(specs) != len(values):
        raise ProtoError("expected %d values for %r, got %d" % (len(specs), sig, len(values)))
    return [_wrap(s, v) for s, v in zip(specs, values)]


_WRAP = {"y": Y, "q": Q, "u": int, "i": I, "a": A, "U": U, "s": str, "b": bytes, "B": bool}


def _wrap(spec, v):
    if isinstance(spec, str):
        if spec == "B":
            return bool(v)
        if spec == "b":
            return bytes(v)
        return _WRAP[spec](v)
    if spec[0] == "T":
        return tuple(_wrap(s, x) for s, x in zip(spec[1], v))
    if len(spec) == 3:
        return [_wrap(spec[1][0], x) for x in v]
    return [tuple(_wrap(s, x) for s, x in zip(spec[1], item)) for item in v]


def message(op, msg_id, values):
    """Encode op with plain values, typed by OPS' signature (FLAG_FD set for
    the ops that carry an fd)."""
    return encode(op, msg_id, typed(OPS[op][1], values), FLAG_FD if op in FD_OPS else 0)


def connect_signal(addr, channel, status):
    """What Android's BluetoothSocket reads first from an RFCOMM stream: the
    channel (int), then sock_connect_signal_t (packed, 20 bytes: size, address,
    channel, status, max tx and rx packet sizes). Native order: little endian."""
    return (struct.pack("<i", channel) +
            struct.pack("<h6siiHH", 20, addr_bytes(addr), channel, status, 0, 0))


def reply(request_op, msg_id, status, values=()):
    sig = OPS[request_op][2] if status == ST_OK else ""
    if status == ST_ATT:
        sig = "y"
    return encode("REPLY", msg_id, [int(status)] + typed(sig, list(values)))


BASE_UUID = "-0000-1000-8000-00805f9b34fb"


def full_uuid(u):
    u = u.lower()
    if len(u) == 4:
        return "0000" + u + BASE_UUID
    if len(u) == 8:
        return u + BASE_UUID
    return str(_uuid.UUID(u))


def short_uuid(u):
    """16-bit form ('180f') of a SIG UUID, else None."""
    u = full_uuid(u)
    if u.endswith(BASE_UUID) and u.startswith("0000"):
        return u[4:8]
    return None
