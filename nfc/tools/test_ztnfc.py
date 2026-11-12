#!/usr/bin/env python3
"""Host test for ztnfc.py: a simulated S3NRN82 answers on a socketpair, and
every packet ztnfc sends is checked against the kernel allowlist
(nci-filter.h, compiled into a small shared library).

    python3 -I nfc/tools/test_ztnfc.py
"""
import contextlib
import ctypes
import io
import os
import socket
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ztnfc  # noqa: E402

FAKE_UID = bytes([0x04, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6])


def build_filter():
    d = tempfile.mkdtemp()
    c = os.path.join(d, "f.c")
    so = os.path.join(d, "f.so")
    with open(c, "w") as f:
        f.write('#include "nci-filter.h"\n'
                'const char *check(const unsigned char *p, unsigned int n, unsigned char pr)'
                '{ return zrn_nci_check(p, n, pr); }\n')
    subprocess.check_call(["cc", "-shared", "-fPIC", "-I", os.path.join(HERE, "../driver"),
                           "-o", so, c])
    lib = ctypes.CDLL(so)
    lib.check.restype = ctypes.c_char_p
    lib.check.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_ubyte]
    return lib


def ntag213_memory():
    mem = bytearray(45 * 4)
    mem[0:3] = FAKE_UID[0:3]
    mem[4:8] = FAKE_UID[3:7]
    mem[12:16] = bytes([0xE1, 0x10, 0x12, 0x00])	# CC: 144-byte data area
    rec = bytes([0xD1, 0x01, 0x08, ord("U"), 0x04]) + b"x.test"
    mem[16:16 + 2 + len(rec) + 1] = bytes([0x03, len(rec)]) + rec + b"\xfe"
    return mem


class Chip(threading.Thread):
    """the controller side: answers commands, enforces the filter"""

    def __init__(self, sock, flt, card):
        super().__init__(daemon=True)
        self.s, self.flt, self.card = sock, flt, card
        self.proto = 0
        self.refused = []
        self.mem = ntag213_memory()

    def tx(self, b):
        self.s.send(bytes(b))

    def run(self):
        while True:
            try:
                p = self.s.recv(300)
            except OSError:
                return
            if not p:
                return
            why = self.flt.check(p, len(p), self.proto)
            if why:
                self.refused.append((p.hex(), why.decode()))
                continue
            self.handle(p)

    def handle(self, p):
        if p[0] == 0x20 and p[1] == 0x00:
            self.tx([0x40, 0x00, 0x03, 0x00, 0x10, 0x01])
        elif p[0] == 0x20 and p[1] == 0x01:
            self.tx(bytes.fromhex("400114000" "20e020003010203022e04ff10000e45004b02"))
        elif p[0] == 0x20 and p[1] == 0x02:
            self.tx([0x40, 0x02, 0x02, 0x00, 0x00])
        elif p[0] == 0x21 and p[1] == 0x00:
            self.tx([0x41, 0x00, 0x01, 0x00])
        elif p[0] == 0x21 and p[1] == 0x03:
            self.tx([0x41, 0x03, 0x01, 0x00])
            self.activate()
        elif p[0] == 0x21 and p[1] == 0x06:
            self.proto = 0
            self.tx([0x41, 0x06, 0x01, 0x00])
            self.tx([0x61, 0x06, 0x02, 0x00, 0x00])
        elif p[0] == 0x00:
            self.data(p[3:])

    def activate(self):
        if self.card == "ntag":
            tp = bytes([0x44, 0x00, 7]) + FAKE_UID + bytes([1, 0x00])
            body = bytes([1, 0x01, 0x02, 0x00, 0xFF, 1, len(tp)]) + tp + bytes([0, 0, 0, 0])
            self.proto = 0x02
        else:	# ISO-DEP DESFire-like
            tp = bytes([0x44, 0x03, 7]) + FAKE_UID + bytes([1, 0x20])
            ats = bytes([0x06, 0x75, 0x77, 0x81, 0x02, 0x80])
            body = bytes([1, 0x02, 0x04, 0x00, 0xFF, 1, len(tp)]) + tp + \
                bytes([0, 0, 0, len(ats) + 1, len(ats)]) + ats
            self.proto = 0x04
        self.tx(bytes([0x61, 0x05, len(body)]) + body)

    def data(self, q):
        if self.proto == 0x02:
            if q[0] == 0x30:
                pg = q[1]
                out = bytes(self.mem[pg * 4:pg * 4 + 16]).ljust(16, b"\0") + b"\x00"
            else:
                out = bytes([0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x0F, 0x03, 0x00])
        else:
            if q[:2] == b"\x00\xa4":
                out = b"\x6a\x82"
            elif q[1] == 0x60:
                out = bytes([0x04, 0x01, 0x01, 0x01, 0x00, 0x18, 0x05, 0x91, 0xAF])
            elif q[1] == 0xAF and not hasattr(self, "af"):
                self.af = 1
                out = bytes([0x04, 0x01, 0x01, 0x01, 0x04, 0x18, 0x05, 0x91, 0xAF])
            else:
                out = FAKE_UID + bytes([1, 2, 3, 4, 5, 0x20, 0x19, 0x91, 0x00])
        self.tx(bytes([0x00, 0x00, len(out)]) + out)
        self.tx([0x60, 0x06, 0x03, 0x01, 0x00, 0x01])


def run(card, flt):
    a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    chip = Chip(b, flt, card)
    chip.start()

    class FakeNci(ztnfc.Nci):
        def __init__(self, dev):
            self.fd = os.dup(a.fileno())
            a.settimeout(None)
            self.pending = []
            self.credits = 0xFF

        def _read(self):
            import select
            r, _, _ = select.select([self.fd], [], [], 0.2)
            return os.read(self.fd, 300) if r else None

    ztnfc.Nci = FakeNci
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = ztnfc.scan("/dev/null", 5)
    out = buf.getvalue()
    a.close()
    b.close()
    return rc, out, chip.refused


def main():
    flt = build_filter()
    fails = 0
    uid_hex = FAKE_UID.hex()
    uid_sp = " ".join(f"{x:02x}" for x in FAKE_UID)
    for card, want in (("ntag", ["NTAG213", "data area 144 bytes", "URI",
                                 "atqa=0044", "sak=00", "field off"]),
                       ("desfire", ["DESFire GetVersion: HW vendor 04", "ats_len=6",
                                    "not an EMV card", "frame 3 UID/batch redacted"])):
        rc, out, refused = run(card, flt)
        print(f"--- {card} (rc {rc})\n{out}")
        for w in want:
            if w not in out:
                print(f"FAIL {card}: missing '{w}'")
                fails += 1
        if uid_hex in out.replace(" ", "") or uid_sp in out or "x.test" in out:
            print(f"FAIL {card}: identifier or content leaked")
            fails += 1
        if refused:
            print(f"FAIL {card}: refused by the allowlist: {refused}")
            fails += 1
    print("PASS" if not fails else f"FAIL ({fails})")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
