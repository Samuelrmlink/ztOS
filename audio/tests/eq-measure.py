#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Measure the Sound app's equalizer on the phone, quietly (ROADMAP 8.67).

Plays two tones (1 kHz and 4 kHz, each -36 dBFS, -30 dBFS peak together) to
the default sink, records a sink's monitor with parecord, and reads each
tone's level with a Goertzel filter. Then sets one band through
ztos-soundd's socket, measures again, and puts the band back as it was.

    eq-measure.py [--monitor SINK] [--band 5] [--gain -12] [--profile auto]

SINK defaults to the output ztos-soundd plays into (its state file's
"target"), else the default sink. Pure Python: the phone has no numpy.
"""
import argparse
import json
import math
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

RUN = os.environ.get("XDG_RUNTIME_DIR", "/run/user/%d" % os.getuid())
SOCK = os.path.join(RUN, "ztos-sound.sock")
STATE = os.path.join(RUN, "ztos-sound-state.json")
RATE = 48000
TONES = (1000.0, 4000.0)
LEVEL_DB = -36.0


def state():
    try:
        with open(STATE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def default_sink():
    return subprocess.check_output(["pactl", "get-default-sink"], text=True).strip()


def write_tone(path, seconds):
    a = 10 ** (LEVEL_DB / 20.0)
    n = int(RATE * seconds)
    fade = RATE // 20
    frames = bytearray()
    for i in range(n):
        env = min(1.0, i / fade, (n - 1 - i) / fade)
        v = sum(a * math.sin(2 * math.pi * f * i / RATE) for f in TONES) * env
        s = int(max(-1.0, min(1.0, v)) * 32767)
        frames += struct.pack("<hh", s, s)
    hdr = b"RIFF" + struct.pack("<I", 36 + len(frames)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 1, 2, RATE, RATE * 4, 4, 16) + b"data" + struct.pack("<I", len(frames))
    with open(path, "wb") as f:
        f.write(hdr + frames)


def goertzel_db(x, f):
    w = 2 * math.pi * f / RATE
    c = 2 * math.cos(w)
    s1 = s2 = 0.0
    for v in x:
        s1, s2 = v + c * s1 - s2, s1
    p = s1 * s1 + s2 * s2 - c * s1 * s2
    amp = 2 * math.sqrt(max(p, 0.0)) / len(x)
    return 20 * math.log10(max(amp, 1e-9))


def measure(monitor, wav, seconds):
    """Each tone's level (dBFS, left channel) and the RMS, at the monitor."""
    with tempfile.NamedTemporaryFile(suffix=".raw", delete=False) as t:
        raw = t.name
    rec = subprocess.Popen(["parecord", "--raw", "--format=float32le", "--rate=%d" % RATE,
                            "--channels=2", "--latency-msec=20", "--device=%s.monitor" % monitor, raw],
                           stderr=subprocess.DEVNULL)
    time.sleep(0.4)
    subprocess.run(["pw-play", wav], check=False)
    time.sleep(0.5)
    rec.send_signal(signal.SIGINT)
    rec.wait()
    with open(raw, "rb") as f:
        data = f.read()
    os.unlink(raw)
    n = len(data) // 8
    left = struct.unpack("<%df" % (2 * n), data[:n * 8])[0::2]
    # the middle of the tone: past its fade and the chain's latency
    thr = 10 ** ((LEVEL_DB - 30) / 20.0)
    idx = [i for i, v in enumerate(left) if abs(v) > thr]
    if len(idx) < RATE // 2:
        return None
    a, b = idx[0] + RATE // 4, idx[-1] - RATE // 4
    seg = left[a:b][: RATE]   # one second is plenty
    rms = math.sqrt(sum(v * v for v in seg) / len(seg))
    return {"1k": goertzel_db(seg, TONES[0]), "4k": goertzel_db(seg, TONES[1]),
            "rms": 20 * math.log10(max(rms, 1e-9))}


def send(line):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        s.sendto(line.encode(), SOCK)
        return True
    except OSError as e:
        print("  (no ztos-soundd: %s)" % e)
        return False
    finally:
        s.close()


def show(tag, m):
    if m is None:
        print("%-10s no signal at the monitor" % tag)
    else:
        print("%-10s 1 kHz %7.2f dBFS   4 kHz %7.2f dBFS   1k-4k %+6.2f dB   rms %7.2f dBFS"
              % (tag, m["1k"], m["4k"], m["1k"] - m["4k"], m["rms"]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--monitor")
    ap.add_argument("--band", type=int, default=5)       # 1 kHz
    ap.add_argument("--gain", type=float, default=-12.0)
    ap.add_argument("--profile", default="auto")
    ap.add_argument("--seconds", type=float, default=2.0)
    a = ap.parse_args()
    st = state()
    chain = bool(st and st.get("chain"))
    monitor = a.monitor or (st.get("target") if chain else None) or default_sink()
    profile = a.profile if a.profile != "auto" else (st.get("active") if chain else "speaker")
    print("default sink %s, ztos-soundd chain %s, monitor %s.monitor, profile %s"
          % (default_sink(), "up" if chain else "down", monitor, profile))
    old = None
    if chain:
        old = st["config"]["profiles"][profile]["bands"][a.band]
    f = old["f"] if old else [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000][a.band]
    q = old["q"] if old else 1.0
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as t:
        wav = t.name
    write_tone(wav, a.seconds)
    try:
        base = measure(monitor, wav, a.seconds)
        show("before", base)
        send("p %s band %d %.2f %.2f %.3f" % (profile, a.band, f, a.gain, q))
        time.sleep(0.5)
        after = measure(monitor, wav, a.seconds)
        show("band %+g" % a.gain, after)
    finally:
        if old:
            send("p %s band %d %.2f %.2f %.3f" % (profile, a.band, old["f"], old["g"], old["q"]))
            # until the state file says so: a run straight after this one
            # would otherwise take the changed band for the one to put back
            for _ in range(30):
                time.sleep(0.1)
                st = state()
                if st and abs(st["config"]["profiles"][profile]["bands"][a.band]["g"] - old["g"]) < 1e-6:
                    break
        os.unlink(wav)
    if base and after:
        d1, d4 = after["1k"] - base["1k"], after["4k"] - base["4k"]
        print("change    1 kHz %+6.2f dB   4 kHz %+6.2f dB   1k relative to 4k %+6.2f dB"
              % (d1, d4, d1 - d4))
        return 0 if abs((d1 - d4) - a.gain) < 3.0 else 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
