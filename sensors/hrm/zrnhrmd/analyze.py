#!/usr/bin/env python3
"""analyze: summarise a /dev/zrn_hrm capture (24-byte records) on the host.

    python3 -I sensors/hrm/zrnhrmd/analyze.py rec.bin [--skip 1.0] [--plot out.svg]
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppg  # noqa: E402

REC = struct.Struct("<QIIII")


def stats(name, x):
    m = sum(x) / len(x)
    bp = ppg.bandpass(x)
    ac = (sum(v * v for v in bp) / len(bp)) ** 0.5
    print(f"{name:4s} min {min(x):6d}  max {max(x):6d}  DC {m:9.1f}  AC rms {ac:8.1f}"
          f"  AC/DC {100 * ac / m if m else 0:.3f} %  (full scale 524287)")


def svg(path, ir, red):
    w, h = 1000, 300
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}">',
           '<rect width="100%" height="100%" fill="white"/>']
    for k, (x, col) in enumerate(((ir, "#c00"), (red, "#06c"))):
        bp = [-v for v in ppg.bandpass(x)]
        m = max(1e-9, max(abs(v) for v in bp))
        y0 = h * (0.25 + 0.5 * k)
        pts = " ".join(f"{i * w / (len(bp) - 1):.1f},{y0 - v / m * h * 0.22:.1f}" for i, v in enumerate(bp))
        out.append(f'<polyline fill="none" stroke="{col}" stroke-width="1.2" points="{pts}"/>')
    out.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rec")
    ap.add_argument("--skip", type=float, default=1.0, help="seconds dropped at the start (settling)")
    ap.add_argument("--plot")
    a = ap.parse_args()
    data = open(a.rec, "rb").read()
    recs = [REC.unpack_from(data, i) for i in range(0, len(data) - REC.size + 1, REC.size)]
    if not recs:
        print("no records")
        return 1
    seqs = [r[1] for r in recs]
    gaps = sum(1 for p, q in zip(seqs, seqs[1:]) if q != p + 1)
    dur = (recs[-1][0] - recs[0][0]) / 1e9
    print(f"{len(recs)} records over {dur:.2f} s ({len(recs) / dur if dur else 0:.1f}/s), "
          f"seq {seqs[0]}..{seqs[-1]}, {gaps} gaps")
    print("first 5 raw (seq, ir, red):", [(r[1], r[2], r[3]) for r in recs[:5]])
    recs = recs[int(a.skip * ppg.RATE):]
    ir = [r[2] for r in recs]
    red = [r[3] for r in recs]
    stats("IR", ir)
    stats("red", red)
    bpm, q, pk = ppg.heart_rate(ir)
    print(f"IR beats found: {len(pk)}; heart rate {bpm and round(bpm, 1)} bpm, quality {q:.2f}")
    bpm_r, q_r, _ = ppg.heart_rate(red)
    print(f"red channel alone: {bpm_r and round(bpm_r, 1)} bpm, quality {q_r:.2f}")
    sp, r, pi = ppg.spo2(ir, red)
    print(f"R {r and round(r, 3)}, perfusion index {pi:.2f} %, SpO2 (uncalibrated 110-25R) "
          f"{sp and round(sp, 1)}")
    if a.plot:
        svg(a.plot, ir, red)
        print("plot:", a.plot)
    return 0


if __name__ == "__main__":
    sys.exit(main())
