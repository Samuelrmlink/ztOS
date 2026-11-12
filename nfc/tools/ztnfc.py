#!/usr/bin/env python3
"""ztnfc: identify and read one NFC card through /dev/zt-nfc (zrn-nfc).

Stage 2, owner-approved 2026-11-12. The kernel enforces the command
allowlist (nfc/driver/nci-filter.h); this tool only uses commands on it:
CORE_RESET/INIT, CORE_SET_CONFIG (500 ms, PA_BAIL_OUT), RF_DISCOVER_MAP
(ISO-DEP poll), RF_DISCOVER (poll A/B/F/V), RF_DISCOVER_SELECT, RF_DEACTIVATE,
and read-only card commands (T2T READ/GET_VERSION, ISO-DEP SELECT NDEF/PPSE,
DESFire GetVersion, FeliCa Polling/Read Without Encryption).

Privacy: UIDs (NFCID1/NFCID2/PUPI/15693 UID), PANs and names are never
printed; raw frames that carry them are logged with those bytes redacted.

    ztnfc.py scan [--seconds 20] [--device /dev/zt-nfc]
"""
import errno
import os
import sys
import time

RF_LIMIT = 25.0		# seconds of field before RF_DEACTIVATE (kernel cuts at 30)


def hx(b):
    return " ".join(f"{x:02x}" for x in b)


def log(*a):
    print(*a, flush=True)


class Nci:
    def __init__(self, dev):
        self.fd = os.open(dev, os.O_RDWR)
        self.pending = []
        self.credits = 0xFF			# 0xFF = no flow control

    def close(self):
        os.close(self.fd)

    def send(self, pkt, show=True):
        pkt = bytes(pkt)
        os.write(self.fd, pkt)
        if show:
            log(f"TX {hx(pkt)}")

    def _read(self):
        try:
            return os.read(self.fd, 300)
        except OSError as e:
            if e.errno == errno.EAGAIN:
                return None
            raise

    def wait(self, pred, timeout):
        """return the first packet for which pred(pkt) is true; keep others"""
        for i, p in enumerate(self.pending):
            if pred(p):
                return self.pending.pop(i)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            p = self._read()
            if p is None:
                continue
            if pred(p):
                return p
            self.pending.append(p)
        return None

    def rsp(self, gid_oid, timeout=1.0):
        g, o = gid_oid
        p = self.wait(lambda p: p[0] & 0xEF == 0x40 | g and p[1] & 0x3F == o, timeout)
        if p is not None:
            log(f"RX {hx(p)}")
        return p

    def _take_credits(self):
        keep = []
        for q in self.pending:
            if q[0] == 0x60 and q[1] == 0x06 and len(q) >= 6:	# conn, credits
                if self.credits != 0xFF:
                    self.credits += q[5]
            else:
                keep.append(q)
        self.pending = keep

    def data(self, payload, timeout=1.0):
        """send on conn 0 (NCI credit flow control), return the data payload"""
        self._take_credits()
        if self.credits == 0:
            p = self.wait(lambda p: p[0] == 0x60 and p[1] == 0x06, 1.0)
            if p is None:
                log("no NCI credit returned; not sending")
                return None
            self.pending.append(p)
            self._take_credits()
        self.send(bytes([0x00, 0x00, len(payload)]) + bytes(payload), show=False)
        if self.credits != 0xFF:
            self.credits -= 1
        p = self.wait(lambda p: p[0] >> 5 == 0, timeout)
        self._take_credits()
        return None if p is None else p[3:]


# ---------------- activation parsing (redacted) ----------------

TECH = {0x00: "NFC-A", 0x01: "NFC-B", 0x02: "NFC-F", 0x06: "NFC-V"}
PROTO = {0x01: "T1T", 0x02: "T2T", 0x03: "T3T (FeliCa)", 0x04: "ISO-DEP",
         0x05: "NFC-DEP", 0x80: "ISO 15693 (prop)", 0x81: "MIFARE Classic (prop)"}
IFACE = {0x01: "Frame", 0x02: "ISO-DEP", 0x03: "NFC-DEP"}


def sak_guess(atqa, sak):
    if sak == 0x00 and atqa == 0x0044:
        return "MIFARE Ultralight / NTAG family (Type 2)"
    if sak in (0x08, 0x88):
        return "MIFARE Classic 1K (or Plus SL1)"
    if sak == 0x18:
        return "MIFARE Classic 4K (or Plus SL1)"
    if sak == 0x09:
        return "MIFARE Mini"
    if sak == 0x20:
        return "ISO 14443-4 (DESFire / Plus SL3 / EMV / other)"
    if sak in (0x10, 0x11):
        return "MIFARE Plus SL2"
    return "unknown"


def parse_activation(p):
    q = p[3:]
    info = {"rf_id": q[0], "iface": q[1], "proto": q[2], "tech": q[3],
            "max_payload": q[4], "credits": q[5]}
    tl = q[6]
    t = q[7:7 + tl]
    rest = q[7 + tl:]
    al = rest[3] if len(rest) > 3 else 0
    act = rest[4:4 + al] if len(rest) > 4 else b""
    tech = q[3] & 0x7F
    red = bytearray(p)
    if tech == 0x00 and len(t) >= 3:	# NFC-A: SENS_RES(2) NFCID1 len, NFCID1, SEL_RES len, SEL_RES
        atqa = t[0] | t[1] << 8
        n = t[2]
        info["atqa"] = f"{atqa:04x}"
        info["uid_len"] = n
        if len(t) > 3 + n and t[3 + n] >= 1:
            info["sak"] = f"{t[4 + n]:02x}"
            info["guess"] = sak_guess(atqa, t[4 + n])
        info["_uid"] = bytes(t[3:3 + n])
        for i in range(3 + 7 + 3, 3 + 7 + 3 + n):
            red[i] = 0xEE
    elif tech == 0x01 and len(t) >= 1:	# NFC-B: SENSB_RES len, SENSB_RES (50 PUPI(4) app(4) proto(3))
        sb = t[1:1 + t[0]]
        info["pupi_len"] = 4
        if len(sb) >= 12:
            info["app_data"] = hx(sb[5:9])
            info["proto_info"] = hx(sb[9:12])
        for i in range(3 + 7 + 2, 3 + 7 + 6):
            red[i] = 0xEE
    elif tech == 0x02 and len(t) >= 2:	# NFC-F: bitrate, SENSF_RES len, SENSF_RES (01 NFCID2 PAD [RD])
        sf = t[2:2 + t[1]]			# NFCID2(8) PAD(8) [RD(2)], as AOSP nfc_ncif
        info["bitrate"] = {1: "212", 2: "424"}.get(t[0], hex(t[0]))
        if len(sf) >= 16:
            info["_nfcid2"] = bytes(sf[0:8])
            info["pad"] = hx(sf[8:16])
            if len(sf) >= 18:
                info["system_code"] = f"{sf[16]:02x}{sf[17]:02x}"
        for i in range(3 + 7 + 2, min(len(red), 3 + 7 + 10)):
            red[i] = 0xEE
    elif len(t) >= 10:			# NFC-V: flags, DSFID, UID(8, LSB first)
        info["dsfid"] = f"{t[1]:02x}"
        info["ic_mfr"] = f"{t[2 + 6]:02x}"
        for i in range(3 + 7 + 2, 3 + 7 + 10):
            red[i] = 0xEE
    if q[1] == 0x02 and tech == 0x00 and len(act) >= 1:
        info["ats_len"] = act[0]
        red[len(red) - len(act):] = b"\xee" * len(act)
    log(f"RX {hx(red)}   (identifiers redacted as ee)")
    return info


def show(info):
    out = {k: v for k, v in info.items() if not k.startswith("_")}
    out["tech"] = TECH.get(info["tech"] & 0x7F, hex(info["tech"]))
    out["proto"] = PROTO.get(info["proto"], hex(info["proto"]))
    out["iface"] = IFACE.get(info["iface"], hex(info["iface"]))
    log("CARD " + ", ".join(f"{k}={v}" for k, v in out.items()))


# ---------------- per-protocol readers ----------------

NTAG = {0x0F: "NTAG213", 0x11: "NTAG215", 0x13: "NTAG216", 0x0B: "Ultralight EV1 (48 B)",
        0x0E: "Ultralight EV1 (128 B)", 0x12: "NTAG210u/212?"}


def read_t2t(nci):
    pages = {}
    r = nci.data([0x30, 0x03])			# CC is page 3; READ returns pages 3..6
    if r is None or len(r) < 17:
        log(f"T2T READ 3: {'no answer' if r is None else 'short/NAK ' + hx(r)}")
        return
    cc = r[0:4]
    log(f"T2T CC (page 3) = {hx(cc)}: magic {'ok' if cc[0] == 0xE1 else 'none'}, "
        f"version {cc[1] >> 4}.{cc[1] & 15}, data area {cc[2] * 8} bytes, access {cc[3]:02x}")
    size = cc[2] * 8 if cc[0] == 0xE1 else 48
    last = 4 + size // 4			# first page after the data area
    for pg in range(4, last, 4):
        r = nci.data([0x30, pg])
        if r is None or len(r) < 17:
            log(f"T2T READ {pg}: stop ({'no answer' if r is None else hx(r)})")
            break
        for i in range(4):
            pages[pg + i] = bytes(r[4 * i:4 * i + 4])
    data = b"".join(pages[p] for p in sorted(pages) if p < last)
    log(f"T2T read {len(data)} bytes of the data area; "
        f"{sum(1 for p in pages if pages[p] != bytes(4))} non-empty pages")
    ndef_summary(data)
    v = nci.data([0x60])
    if v is not None and len(v) >= 9:
        st = v[6]
        log(f"T2T GET_VERSION = {hx(v[:8])}: vendor {v[1]:02x} type {v[2]:02x} "
            f"subtype {v[3]:02x} v{v[4]}.{v[5]} storage {st:02x} "
            f"({NTAG.get(st, '?')}) protocol {v[7]:02x}")
    else:
        log(f"T2T GET_VERSION: {'no answer' if v is None else 'NAK ' + hx(v)} "
            "(plain Ultralight / Ultralight C do not support it)")


def ndef_summary(data):
    i = 0
    while i < len(data):
        t = data[i]
        if t == 0x00:
            i += 1
            continue
        if t == 0xFE:
            log("NDEF: terminator TLV")
            return
        if i + 1 >= len(data):
            return
        ln = data[i + 1]
        hdr = 2
        if ln == 0xFF and i + 3 < len(data):
            ln = data[i + 2] << 8 | data[i + 3]
            hdr = 4
        if t == 0x03:
            body = data[i + hdr:i + hdr + ln]
            if len(body) >= 3:
                h = body[0]
                tl = body[1]
                sr = h & 0x10
                off = 3 if sr else 6
                if h & 0x08:
                    off += 1
                rtype = bytes(body[off:off + tl])
                kind = {b"U": "URI", b"T": "Text", b"Sp": "Smart Poster"}.get(rtype, None)
                log(f"NDEF: message TLV, {ln} bytes, first record TNF {h & 7} type "
                    f"{kind or ('mime/ext, ' + str(tl) + ' bytes')} (content not shown)")
            else:
                log(f"NDEF: message TLV, {ln} bytes (empty)")
            return
        log(f"T2T TLV type {t:02x}, {ln} bytes")
        i += hdr + ln
    log("NDEF: none")


def tlv_find(b, tags, out):
    i = 0
    while i < len(b):
        tag = b[i]
        i += 1
        if tag & 0x1F == 0x1F and i < len(b):
            tag = tag << 8 | b[i]
            i += 1
        if i >= len(b):
            break
        ln = b[i]
        i += 1
        if ln == 0x81 and i < len(b):
            ln = b[i]
            i += 1
        v = b[i:i + ln]
        if tag in tags:
            out.append((tag, bytes(v)))
        if tag in (0x6F, 0xA5, 0xBF0C, 0x61):
            tlv_find(v, tags, out)
        i += ln


def read_isodep(nci):
    aid_ndef = [0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01]
    r = nci.data([0x00, 0xA4, 0x04, 0x00, len(aid_ndef)] + aid_ndef + [0x00])
    log(f"ISO-DEP SELECT NDEF app: SW {'none' if not r else hx(r[-2:])}"
        f"{' -> NDEF Type 4 app present' if r and r[-2:] == b'\x90\x00' else ''}")
    ppse = list(b"2PAY.SYS.DDF01")
    r = nci.data([0x00, 0xA4, 0x04, 0x00, len(ppse)] + ppse + [0x00])
    if r and r[-2:] == b"\x90\x00":
        found = []
        tlv_find(r[:-2], (0x4F, 0x50, 0x87), found)
        apps = []
        for tag, v in found:
            if tag == 0x4F:
                apps.append(f"AID {v.hex().upper()}")
            elif tag == 0x50:
                apps.append(f"label '{v.decode('ascii', 'replace')}'")
        log("ISO-DEP SELECT PPSE: 9000, EMV payment card; " + ("; ".join(apps) or "no apps listed"))
    else:
        log(f"ISO-DEP SELECT PPSE: SW {'none' if not r else hx(r[-2:])} (not an EMV card)")
    r = nci.data([0x90, 0x60, 0x00, 0x00, 0x00])
    if r and r[-2:] == b"\x91\xaf":
        hw = r[:-2]
        r2 = nci.data([0x90, 0xAF, 0x00, 0x00, 0x00])
        r3 = nci.data([0x90, 0xAF, 0x00, 0x00, 0x00]) if r2 and r2[-2:] == b"\x91\xaf" else None
        sz = hw[5] if len(hw) > 5 else 0
        log(f"DESFire GetVersion: HW vendor {hw[0]:02x} type {hw[1]:02x} subtype {hw[2]:02x} "
            f"v{hw[3]}.{hw[4]} storage {sz:02x} (~{1 << (sz >> 1)} bytes) protocol "
            f"{hw[6] if len(hw) > 6 else 0:02x}")
        if r2:
            sw = r2[:-2]
            log(f"DESFire GetVersion: SW v{sw[3]}.{sw[4]} (frame 2)")
        if r3 and len(r3) >= 16:
            log(f"DESFire GetVersion: frame 3 UID/batch redacted, production week {r3[12]:02x} "
                f"year {r3[13]:02x}")
    else:
        log(f"DESFire GetVersion: SW {'none' if not r else hx(r[-2:])} (not DESFire)")


def read_t3t(nci, info):
    r = nci.data([0x06, 0x00, 0xFF, 0xFF, 0x01, 0x00])
    if r and len(r) >= 20 and r[1] == 0x01:
        log(f"FeliCa Polling (system code request): system code {r[18]:02x}{r[19]:02x}")
    else:
        log(f"FeliCa Polling: {'no answer' if r is None else 'len ' + str(len(r))}")
    idm = info.get("_nfcid2")
    if not idm:
        return
    cmd = [0x10, 0x06] + list(idm) + [0x01, 0x0B, 0x00, 0x01, 0x80, 0x00]
    r = nci.data(cmd)
    if r and len(r) >= 12 and r[1] == 0x07:
        sf1, sf2 = r[10], r[11]
        msg = f"FeliCa Read Without Encryption, service 000B block 0: status {sf1:02x} {sf2:02x}"
        if sf1 == 0 and len(r) >= 13 + 16:
            b = r[13:29]
            ln = b[11] << 16 | b[12] << 8 | b[13]
            msg += f" -> NDEF attribute block: version {b[0] >> 4}.{b[0] & 15}, Nbr {b[1]}, " \
                   f"Nbw {b[2]}, Nmaxb {b[3] << 8 | b[4]}, NDEF length {ln}"
        log(msg)
    else:
        log(f"FeliCa Read Without Encryption: {'no answer' if r is None else 'len ' + str(len(r))}")


# ---------------- session ----------------

def scan(dev, seconds):
    nci = Nci(dev)
    try:
        nci.send([0x20, 0x00, 0x01, 0x00])
        r = nci.rsp((0, 0))
        if not r or r[3] != 0:
            log("CORE_RESET failed")
            return 1
        nci.send([0x20, 0x01, 0x00])
        r = nci.rsp((0, 1))
        if not r or r[3] != 0:
            log("CORE_INIT failed")
            return 1
        nci.send([0x20, 0x02, 0x08, 0x02, 0x00, 0x02, 0xF4, 0x01, 0x08, 0x01, 0x01])
        r = nci.rsp((0, 2))
        if not r or r[3] != 0:
            log("CORE_SET_CONFIG failed")
            return 1
        nci.send([0x21, 0x00, 0x04, 0x01, 0x04, 0x01, 0x02])
        r = nci.rsp((1, 0))
        if not r or r[3] != 0:
            log("RF_DISCOVER_MAP failed")
            return 1
        nci.send([0x21, 0x03, 0x09, 0x04, 0x00, 0x01, 0x01, 0x01, 0x02, 0x01, 0x06, 0x01])
        t0 = time.monotonic()
        r = nci.rsp((1, 3))
        if not r or r[3] != 0:
            log("RF_DISCOVER failed")
            return 1
        log(f"field on, polling A/B/F/V for up to {seconds:.0f} s ...")
        try:
            return session(nci, t0, seconds)
        finally:
            nci.send([0x21, 0x06, 0x01, 0x00])
            nci.rsp((1, 6))
            n = nci.wait(lambda p: p[0] == 0x61 and p[1] == 0x06, 1.0)
            log(f"field off after {time.monotonic() - t0:.1f} s "
                f"({'RF_DEACTIVATE_NTF ' + hx(n) if n else 'no NTF'})")
    finally:
        nci.close()


def session(nci, t0, seconds):
    deadline = t0 + min(seconds, RF_LIMIT)
    found = []
    while time.monotonic() < deadline:
        p = nci.wait(lambda p: p[0] == 0x61 and p[1] in (0x03, 0x05), 0.5)
        if p is None:
            continue
        if p[1] == 0x03:			# RF_DISCOVER_NTF: id, proto, tech, len, params, type
            q = p[3:]
            found.append((q[0], q[1], q[2]))
            log(f"DISCOVER_NTF: id {q[0]} protocol {PROTO.get(q[1], hex(q[1]))} "
                f"tech {TECH.get(q[2] & 0x7F, hex(q[2]))} (identifiers not shown)")
            if q[-1] != 0x02:			# last notification: pick one
                pick = next((f for f in found if f[1] in (0x02, 0x03, 0x04)), None)
                if pick is None:
                    log("no card with an allowed protocol (T2T/T3T/ISO-DEP)")
                    return 0
                nci.send([0x21, 0x04, 0x03, pick[0], pick[1], 0x02 if pick[1] == 0x04 else 0x01])
                nci.rsp((1, 4))
            continue
        info = parse_activation(p)
        nci.credits = info["credits"]
        show(info)
        if info["proto"] == 0x02:
            read_t2t(nci)
        elif info["proto"] == 0x04 and info["iface"] == 0x02:
            read_isodep(nci)
        elif info["proto"] == 0x03:
            read_t3t(nci, info)
        else:
            log("identification only for this protocol (no approved read commands)")
        return 0
    log("no card found")
    return 2


def main(argv):
    dev = "/dev/zt-nfc"
    seconds = 20.0
    args = list(argv)
    if not args or args[0] != "scan":
        print(__doc__)
        return 1
    args.pop(0)
    while args:
        a = args.pop(0)
        if a == "--seconds":
            seconds = float(args.pop(0))
        elif a == "--device":
            dev = args.pop(0)
    return scan(dev, min(seconds, RF_LIMIT))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
