#!/usr/bin/env python3
"""zrn-hrmd: on-demand heart rate and SpO2 from /dev/zrn_hrm (zrn-hrm.ko).

NOT APPROVED TO RUN YET: a measurement switches on the sensor's LEDs.

The sensor is used only during a measurement, never in the background.

Control is a FIFO, /run/zrn-hrmd.ctl, which anyone in the session may write:
    start [SECONDS]   measure for SECONDS (default 30, at most 60)
    stop              end early

While it measures, and once after, it writes /run/zrn-sensors/hrm (atomically,
one key=value per line):
    state=idle|measuring|no-finger|done|stopped|error
                    (no-finger only while measuring; done, stopped and error
                    end a measurement, done with bpm empty when nothing was found)
    bpm=72          (empty until there is a confident rate)
    quality=0.93    (share of beat intervals within 20 % of the median)
    spo2=97         (experimental, uncalibrated; empty when gated out)
    pi=1.4          (IR perfusion index, %)
    elapsed=12  total=30
    error=...       (state=error only)
and /run/zrn-sensors/hrm-wave: the last 4 s of band-passed IR, one value a
line, scaled to -1..1, for the app's trace.

    zrn-hrmd.py [--dev /dev/zrn_hrm] [--replay FILE.bin]
--replay feeds a recorded raw file (the device's 24-byte records) instead of
the device, for testing on a host.
"""
import argparse
import os
import select
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppg  # noqa: E402

REC = struct.Struct("<QIIII")      # t_ns, seq, ir, red, pad (zrn-hrm.h)
CTL = "/run/zrn-hrmd.ctl"
OUT_DIR = os.environ.get("ZRN_HRM_OUT", "/run/zrn-sensors")
WINDOW_S = 8.0
FINGER_DC = 50000                  # IR counts with a finger at 9.6 mA: a guess, to tune on the phone
MAX_S = 60


def publish(name, text):
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, name)
    with open(path + ".new", "w") as f:
        f.write(text)
    os.chmod(path + ".new", 0o644)
    os.replace(path + ".new", path)


def report(state, bpm=None, quality=0.0, sp=None, pi=0.0, elapsed=0, total=0, error=""):
    lines = [f"state={state}",
             f"bpm={round(bpm) if bpm else ''}",
             f"quality={quality:.2f}",
             f"spo2={round(sp) if sp is not None else ''}",
             f"pi={pi:.2f}",
             f"elapsed={int(elapsed)}",
             f"total={int(total)}"]
    if error:
        lines.append(f"error={error}")
    publish("hrm", "\n".join(lines) + "\n")


def wave(ir):
    tail = ir[-int(4 * ppg.RATE):]
    if len(tail) < ppg.RATE:
        return
    bp = [-v for v in ppg.bandpass(tail)]
    m = max(1e-9, max(abs(v) for v in bp))
    publish("hrm-wave", "\n".join(f"{v / m:.3f}" for v in bp) + "\n")


def evaluate(ir, red, now, seconds):
    """One estimate over the window; reports it unless now is None (the end)."""
    if not ppg.finger_on(ir[-int(ppg.RATE):], FINGER_DC):
        if now is not None:
            report("no-finger", elapsed=now, total=seconds)
        return None, 0.0, None, 0.0
    b, q, _ = ppg.heart_rate(ir)
    bpm, quality = (b, q) if b and q >= 0.6 else (None, q)
    sp, _, pi = ppg.spo2(ir, red)
    if not bpm:
        sp = None                                       # no clean pulse, no SpO2
    if now is not None:
        report("measuring", bpm, quality, sp, pi, now, seconds)
    wave(ir)
    return bpm, quality, sp, pi


def measure(source, seconds, ctl_fd):
    """Read records for `seconds`, reporting once a second. Returns the end state."""
    ir, red = [], []
    t0 = time.monotonic()
    last = 0.0
    buf = b""
    bpm, quality, sp, pi = None, 0.0, None, 0.0
    state = "measuring"
    report(state, total=seconds)
    while True:
        now = time.monotonic() - t0
        if now >= seconds:
            break
        fds = [source] + ([ctl_fd] if ctl_fd is not None else [])
        r, _, _ = select.select(fds, [], [], 0.25)
        if ctl_fd in r:
            cmd = os.read(ctl_fd, 256).decode(errors="replace").split()
            if cmd and cmd[0] == "stop":
                state = "stopped"
                break
        if source in r:
            chunk = os.read(source, REC.size * 64)
            if not chunk:
                break                                   # replay file ended
            buf += chunk
            n = len(buf) // REC.size
            for i in range(n):
                _, _, a, b, _ = REC.unpack_from(buf, i * REC.size)
                ir.append(a)
                red.append(b)
            buf = buf[n * REC.size:]
            keep = int(WINDOW_S * ppg.RATE)
            ir, red = ir[-keep:], red[-keep:]
        if now - last >= 1.0 and len(ir) >= ppg.RATE:
            last = now
            bpm, quality, sp, pi = evaluate(ir, red, now, seconds)
    if state == "measuring" and ir:
        bpm, quality, sp, pi = evaluate(ir, red, None, seconds)
    if state == "measuring":
        state = "done"                                  # bpm is empty when there was no reading
    report(state, bpm, quality, sp, pi, min(seconds, time.monotonic() - t0), seconds)
    return state


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev", default="/dev/zrn_hrm")
    ap.add_argument("--replay")
    ap.add_argument("--seconds", type=float, help="with --replay: measure once and exit")
    args = ap.parse_args()

    if args.replay:
        fd = os.open(args.replay, os.O_RDONLY)
        print(measure(fd, args.seconds or 30, None))
        print(open(os.path.join(OUT_DIR, "hrm")).read(), end="")
        return 0

    if not os.path.exists(CTL):
        os.mkfifo(CTL, 0o622)
        os.chmod(CTL, 0o622)
    ctl = os.open(CTL, os.O_RDWR | os.O_NONBLOCK)      # RDWR: never sees EOF
    report("idle")
    while True:
        select.select([ctl], [], [])
        cmd = os.read(ctl, 256).decode(errors="replace").split()
        if not cmd or cmd[0] != "start":
            continue
        try:
            seconds = max(5, min(MAX_S, int(cmd[1]) if len(cmd) > 1 else 30))
        except ValueError:
            seconds = 30
        try:
            dev = os.open(args.dev, os.O_RDONLY | os.O_NONBLOCK)
        except OSError as e:
            report("error", error=e.strerror)
            continue
        try:
            measure(dev, seconds, ctl)
        except OSError as e:
            report("error", error=e.strerror)
        finally:
            os.close(dev)                               # LEDs off, chip down


if __name__ == "__main__":
    sys.exit(main())
