#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fit ztacctd's power profile to the gauge: calib.csv -> power_profile lines.

    fit-calib.py /var/lib/ztacct/calib.csv [--wifi-on MW] [--wifi-off MW]

Each row of calib.csv is a 5-minute window on battery: what the model saw
(awake and asleep seconds, screen seconds and screen x backlight seconds, the
CPU model's joules per cluster, GPU joules, Wi-Fi packets) and what the
coulomb counter measured. Least squares, coefficients kept >= 0 (a column
that comes out negative is dropped and the rest fitted again):

    measured - wifi idle = base_awake * awake + base_suspend * asleep
                           + screen * screen_s + screen_full * screen_bright_s
                           + cpu_scale * (cpu_little_j + cpu_big_j)
                           + gpu_scale * gpu_j + wifi_pkt * packets

Wi-Fi idle is taken as measured (power/README.md), not fitted. No numpy:
normal equations, Gaussian elimination.
"""
import csv
import sys

COLS = [("base_awake_mw", "awake_s", 1000), ("base_suspend_mw", "asleep_s", 1000),
        ("screen_mw", "screen_s", 1000), ("screen_full_mw", "screen_bright_s", 1000),
        ("cpu_scale", "cpu", 1), ("gpu_scale", "gpu_j", 1), ("wifi_pkt_mj", "wifi_packets", 1000)]


def solve(a, b):
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(m[r][c]))
        if abs(m[p][c]) < 1e-12:
            return None
        m[c], m[p] = m[p], m[c]
        for r in range(n):
            if r != c:
                f = m[r][c] / m[c][c]
                m[r] = [x - f * y for x, y in zip(m[r], m[c])]
    return [m[i][n] / m[i][i] for i in range(n)]


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    wifi_on, wifi_off = 315.0, 65.0
    if "--wifi-on" in args:
        wifi_on = float(args[args.index("--wifi-on") + 1])
    if "--wifi-off" in args:
        wifi_off = float(args[args.index("--wifi-off") + 1])
    rows = list(csv.DictReader(open(args[0])))
    data = []
    for r in rows:
        v = {k: float(x) for k, x in r.items() if x not in ("", None)}
        if v.get("measured_j", 0) <= 0 or v.get("boot_s", 0) < 120:
            continue
        v["cpu"] = v["cpu_little_j"] + v["cpu_big_j"]
        on = min(v["screen_s"], v["wifi_assoc_s"])
        y = v["measured_j"] - (wifi_on * on + wifi_off * (v["wifi_assoc_s"] - on)) / 1000
        data.append((v, y))
    print(f"{len(data)} windows")
    cols = [c for c in COLS if any(v[c[1]] > 0 for v, _ in data)]
    while cols:
        a = [[sum(v[ci[1]] * v[cj[1]] for v, _ in data) for cj in cols] for ci in cols]
        b = [sum(v[ci[1]] * y for v, y in data) for ci in cols]
        x = solve(a, b)
        if x is None:
            cols.pop()
            continue
        neg = [i for i, k in enumerate(x) if k < 0]
        if not neg:
            break
        cols.pop(min(neg, key=lambda i: x[i]))
    else:
        print("nothing to fit")
        return 1
    res = [y - sum(k * v[c[1]] for k, c in zip(x, cols)) for v, y in data]
    tot = sum(y for _, y in data)
    print(f"residual: {sum(abs(e) for e in res) / max(1e-9, tot) * 100:.1f}% of the measured joules (mean abs)")
    for k, c in zip(x, cols):
        print(f"{c[0]} {k * c[2]:.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
