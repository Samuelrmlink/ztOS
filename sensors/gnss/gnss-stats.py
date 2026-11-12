#!/usr/bin/env python3
"""GNSS session statistics from NMEA, without positions.

  gnss-stats.py live SECONDS [--save FILE]   read the gate (127.0.0.1:10110)
  gnss-stats.py file FILE                    analyse a saved session
  gnss-stats.py redact < in > out            NMEA with every position blanked

Per constellation: satellites in view (GSV), tracked (SNR > 0), used in the
fix (GSA), median SNR; time to first fix from the connection (or the file's
first line); fix quality; the engine's own accuracy ($PZTFX) and HDOP.

Nothing printed or written by `live` and `file` holds a position. `--save`
keeps the raw stream, which does: write it only to a private place (on the
phone, /run or root's home) and never into the repo; `redact` makes a
shareable copy.
"""
import collections
import json
import socket
import statistics
import sys
import time

TALKERS = {"GP": "GPS", "GL": "GLONASS", "GA": "Galileo", "GB": "BeiDou",
           "BD": "BeiDou", "QZ": "QZSS", "GI": "NavIC", "GQ": "QZSS"}
# NMEA 4.10 GSA field 18, the system ID
SYSTEM_ID = {"1": "GPS", "2": "GLONASS", "3": "Galileo", "4": "BeiDou",
             "5": "QZSS", "6": "NavIC"}


def checksum_ok(line):
    if not line.startswith("$") or "*" not in line:
        return False
    body, _, cs = line[1:].partition("*")
    x = 0
    for ch in body.encode("ascii", "replace"):
        x ^= ch
    return "%02X" % x == cs[:2].upper()


def system_of_prn(prn):
    """Which system a satellite number belongs to, in the numbering the
    S9's engine uses in a GN sentence (sensors/GPS-REA.md section 8)."""
    if 1 <= prn <= 32:
        return "GPS"
    if 33 <= prn <= 64:
        return "SBAS"
    if 65 <= prn <= 96:
        return "GLONASS"
    if 100 <= prn <= 150:
        return "Galileo"
    if 193 <= prn <= 200:
        return "QZSS"
    if 201 <= prn <= 263:
        return "BeiDou"
    return "other"


def nmea_fields(line):
    body = line[1:line.index("*")]
    return body.split(",")


# fields that carry a position, by sentence type (index of the first of a
# lat, N/S, lon, E/W group)
POSITION_AT = {"GGA": 2, "RMC": 3, "GLL": 1, "GNS": 2}


def redact(line):
    """The line with its position blanked (checksum fixed), or None to drop
    it. Proprietary position sentences are dropped whole."""
    line = line.strip()
    if not line.startswith("$") or "*" not in line:
        return None
    f = nmea_fields(line)
    kind = f[0]
    if kind.startswith("P"):
        # $PZTLF carries the last fix; $PZTFX only an accuracy
        return line if kind == "PZTFX" else None
    at = POSITION_AT.get(kind[2:])
    if at is not None:
        for i in range(at, min(at + 4, len(f))):
            f[i] = ""
        if kind[2:] == "GGA" and len(f) > 9:
            f[9] = ""      # altitude
    body = ",".join(f)
    x = 0
    for ch in body.encode("ascii", "replace"):
        x ^= ch
    return "$%s*%02X" % (body, x)


def median(v):
    return statistics.median(v) if v else None


class Stats:
    def __init__(self):
        self.t0 = None
        self.first_fix = None       # seconds from t0
        self.first_acc = None       # seconds from t0 to the first $PZTFX
        self.quality = collections.Counter()
        self.gsv_count = collections.Counter()
        self.gsa_count = collections.Counter()
        self.prns = collections.defaultdict(set)      # system -> PRNs seen
        self.snr = collections.defaultdict(list)      # system -> SNR samples > 0
        self.epochs = []        # per GGA: {system: (view, tracked, used)}
        self.view = {}          # system -> {prn: snr} in the current epoch
        self.used = collections.defaultdict(set)
        self.sats_used_gga = []
        self.hdop = []
        self.acc = []
        self.bad = 0
        self.lines = 0
        self.fix_gaps = []
        self.last_fix_t = None

    def epoch_end(self):
        systems = set(self.view) | set(self.used)
        if systems:
            self.epochs.append({s: (len(self.view.get(s, {})),
                                    sum(1 for v in self.view.get(s, {}).values() if v > 0),
                                    len(self.used.get(s, ()))) for s in systems})
        self.view = {}
        self.used = collections.defaultdict(set)

    def feed(self, line, t):
        line = line.strip()
        if not line:
            return
        if self.t0 is None:
            self.t0 = t
        self.lines += 1
        if not checksum_ok(line):
            self.bad += 1
            return
        f = nmea_fields(line)
        kind, talker = f[0][2:], f[0][:2]
        if f[0] == "PZTFX":
            if len(f) > 1 and f[1]:
                try:
                    self.acc.append(float(f[1]))
                except ValueError:
                    return
                if self.first_acc is None:
                    self.first_acc = t - self.t0
            return
        if kind == "GSV" and len(f) > 3:
            system = TALKERS.get(talker, talker)
            self.gsv_count[talker] += 1
            seen = self.view.setdefault(system, {})
            # 4 satellites per sentence; NMEA 4.10 adds a signal ID at the end
            for i in range(4, len(f) - 3, 4):
                if not f[i].isdigit():
                    continue
                prn = int(f[i])
                snr = int(f[i + 3]) if f[i + 3].isdigit() else 0
                seen[prn] = max(snr, seen.get(prn, 0))
                self.prns[system].add(prn)
                if snr > 0:
                    self.snr[system].append(snr)
        elif kind == "GSA" and len(f) > 15:
            self.gsa_count[talker] += 1
            sysid = SYSTEM_ID.get(f[18]) if len(f) > 18 else None
            for v in f[3:15]:
                if v.isdigit():
                    prn = int(v)
                    system = sysid or (TALKERS.get(talker) if talker != "GN" else None) \
                        or system_of_prn(prn)
                    self.used[system].add(prn)
        elif kind == "GGA" and len(f) > 8:
            self.epoch_end()
            q = f[6] or "0"
            self.quality[q] += 1
            if q != "0":
                if self.first_fix is None:
                    self.first_fix = t - self.t0
                if self.last_fix_t is not None and t - self.last_fix_t > 3:
                    self.fix_gaps.append(round(t - self.last_fix_t, 1))
                self.last_fix_t = t
                if f[7].isdigit():
                    self.sats_used_gga.append(int(f[7]))
                try:
                    self.hdop.append(float(f[8]))
                except ValueError:
                    pass

    def summary(self):
        self.epoch_end()
        systems = sorted(set(self.prns) | {s for e in self.epochs for s in e})
        per = {}
        fix_epochs = self.epochs
        for s in systems:
            rows = [e[s] for e in fix_epochs if s in e]
            used = [r[2] for r in rows]
            per[s] = {
                "prns_seen": len(self.prns.get(s, ())),
                "in_view_median": median([r[0] for r in rows]),
                "tracked_median": median([r[1] for r in rows]),
                "used_median": median(used),
                "used_max": max(used) if used else 0,
                "epochs_used": sum(1 for u in used if u > 0),
                "snr_median": median(self.snr.get(s, [])),
                "snr_max": max(self.snr[s]) if self.snr.get(s) else None,
            }
        acc = sorted(self.acc)
        return {
            "lines": self.lines,
            "bad_checksums": self.bad,
            "epochs": len(self.epochs),
            "ttff_s": None if self.first_fix is None else round(self.first_fix, 1),
            "first_accuracy_s": None if self.first_acc is None else round(self.first_acc, 1),
            "fix_quality": dict(self.quality),
            "gsv_by_talker": dict(self.gsv_count),
            "gsa_by_talker": dict(self.gsa_count),
            "sats_used_gga_median": median(self.sats_used_gga),
            "sats_used_gga_max": max(self.sats_used_gga) if self.sats_used_gga else None,
            "hdop_median": median(self.hdop),
            "accuracy_m_median": median(acc),
            "accuracy_m_p95": acc[int(len(acc) * 0.95)] if acc else None,
            "accuracy_m_last": self.acc[-1] if self.acc else None,
            "fix_gaps_s": self.fix_gaps,
            "systems": per,
        }


def live(seconds, save=None):
    st = Stats()
    out = open(save, "w") if save else None
    t0 = time.monotonic()
    st.t0 = t0
    s = socket.create_connection(("127.0.0.1", 10110))
    s.settimeout(1)
    buf = b""
    while time.monotonic() - t0 < seconds:
        try:
            d = s.recv(4096)
        except socket.timeout:
            continue
        if not d:
            break
        buf += d
        *lines, buf = buf.split(b"\n")
        now = time.monotonic()
        for raw in lines:
            line = raw.decode("ascii", "replace").strip()
            if out:
                out.write("%.3f %s\n" % (now - t0, line))
            st.feed(line, now)
    s.close()
    if out:
        out.close()
    return st.summary()


def from_file(path):
    st = Stats()
    with open(path) as f:
        for n, line in enumerate(f):
            t, _, rest = line.partition(" ")
            try:
                st.feed(rest, float(t))
            except ValueError:
                st.feed(line, float(n))   # bare NMEA: one line a "second"
    return st.summary()


def main(argv):
    if len(argv) >= 2 and argv[0] == "live":
        save = argv[argv.index("--save") + 1] if "--save" in argv else None
        print(json.dumps(live(float(argv[1]), save), indent=1, sort_keys=True))
    elif len(argv) == 2 and argv[0] == "file":
        print(json.dumps(from_file(argv[1]), indent=1, sort_keys=True))
    elif argv[:1] == ["redact"]:
        for line in sys.stdin:
            t, sep, rest = line.partition(" ")
            stamped = sep and not t.startswith("$")
            r = redact(rest if stamped else line)
            if r:
                print("%s %s" % (t, r) if stamped else r)
    else:
        print(__doc__, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
