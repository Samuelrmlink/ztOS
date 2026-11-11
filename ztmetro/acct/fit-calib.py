#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fit ztacctd's power profile to the gauge: calib.csv -> power_profile lines.

    fit-calib.py calib.csv [--journal KLOG] [--wifi-on MW] [--wifi-off MW]
                 [--fix KEY=VALUE]... [--only KEY,KEY...] [--profile FILE]
                 [--report [--windows]] [--holdout-after EPOCH]

--fix holds a coefficient at a measured value (base_awake_mw=1190, say) and
fits the rest. --only fits just those keys (the rest held at --profile's
values, or ztacctd's defaults). --profile reads a power_profile as the
starting values. --report prints how much of the measured energy the old
model (each row's model_j) and the fitted one leave Unaccounted, and where
the rest goes by ztacctd's groups (--windows: per window too).
--holdout-after fits on the windows before EPOCH and reports on the ones
after: how well it predicts what it was not fitted to.

Each row of calib.csv is a 5-minute window on battery: what the model saw
(awake and asleep seconds, screen seconds and screen x backlight seconds, the
CPU model's joules per cluster, GPU joules, Wi-Fi packets, and since
2026-11-11 the accessories' and radios' states, in seconds) and what the
coulomb counter measured. Least squares, coefficients kept >= 0 (a column
that comes out negative is dropped and the rest fitted again):

    measured - wifi idle = base_awake * awake + base_suspend * asleep
                           + screen * screen_s + screen_full * screen_bright_s
                           + cpu_scale * (cpu_little_j + cpu_big_j)
                           + gpu_scale * gpu_j + wifi_pkt * packets
                           + otg * otg_s + usb_host * usb_host_s + usb_dev * usb_dev_s
                           + dp_link * dp_link_s + dp_video * dp_video_s
                           + bt_on * bt_on_s + bt_conn * bt_conn_s

Wi-Fi idle is taken as measured (power/README.md), not fitted.

Rows written before the state columns existed have them empty. --journal
fills them from the kernel's log, `journalctl _TRANSPORT=kernel -o
short-unix` (every boot the journal keeps; the phone's journal is
persistent), by these lines:

    "Linux version"                 a boot: everything off
    max77705 "OTG: boost on/off"    VBUS out to a sink partner (otg_s)
    "usb 1-1...: new ... device"    a device enumerated on the dock's bus,
    "usb 1-1...: USB disconnect"    and gone (usb_dev_s, device-seconds;
                                    usb_host_s while there is any)
    usbdrd "device mode"            the bus gone: all devices gone
    "usb1-port1: unable to enumerate"  the dock's hub never came up (11-09,
                                    a kernel without its drivers): no devices,
                                    only the boost
    exynos9810-dp "pma: on"         the DP link powered; the driver leaves it
                                    on until the next boot (dp_link_s)
    exynos9810-dp "drm: enable/disable"  video out (dp_video_s)

Bluetooth was never logged in a usable way: its columns stay empty (unknown)
for old rows and those rows do not fit them. No numpy: normal equations,
Gaussian elimination.
"""
import csv
import sys

# key, column, profile unit per column unit (mW from J/s: 1000)
COLS = [("base_awake_mw", "awake_s", 1000), ("base_suspend_mw", "asleep_s", 1000),
        ("screen_mw", "screen_s", 1000), ("screen_full_mw", "screen_bright_s", 1000),
        ("cpu_scale", "cpu", 1), ("gpu_scale", "gpu_j", 1), ("wifi_pkt_mj", "wifi_packets", 1000),
        ("otg_mw", "otg_s", 1000), ("usb_host_mw", "usb_host_s", 1000), ("usb_dev_mw", "usb_dev_s", 1000),
        ("dp_link_mw", "dp_link_s", 1000), ("dp_video_mw", "dp_video_s", 1000),
        ("bt_on_mw", "bt_on_s", 1000), ("bt_conn_mw", "bt_conn_s", 1000),
        ("wake_ctxt_uj", "ctxt", 1e6), ("wake_irq_uj", "irqs", 1e6)]
STATE = ["otg_s", "usb_host_s", "usb_dev_s", "usb_active_s", "dp_link_s", "dp_video_s", "bt_on_s", "bt_conn_s",
         "ctxt", "irqs"]
# a coefficient fitted only once this many windows know its column (the
# rest of the fit would otherwise shrink to the few rows that do)
MIN_ROWS = 24
FROM_JOURNAL = ["otg_s", "usb_host_s", "usb_dev_s", "dp_link_s", "dp_video_s"]

# ztacctd's defaults (acct-energy.c), what a row's model_j was made with
BASE_MEASURED = 1190	# mW: 325 mA, screen off, Wi-Fi off, panel asleep (power/README.md)
DEFAULTS = {"base_awake_mw": 1190, "base_suspend_mw": 100, "screen_mw": 250, "screen_full_mw": 1100,
            "cpu_scale": 1, "gpu_scale": 1, "wifi_pkt_mj": 0.15, "wifi_byte_uj": 0.1,
            "otg_mw": 0, "usb_host_mw": 0, "usb_dev_mw": 0, "dp_link_mw": 0, "dp_video_mw": 0,
            "bt_on_mw": 0, "bt_conn_mw": 0, "wake_ctxt_uj": 0, "wake_irq_uj": 0}


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


def journal_states(path):
    """The kernel's log -> intervals of each state: {col: [(t0, t1, weight)]}."""
    ev = []
    for line in open(path, errors="replace"):
        p = line.split(None, 3)
        if len(p) < 4:
            continue
        try:
            ev.append((float(p[0]), p[3]))
        except ValueError:
            continue
    ev.sort(key=lambda e: e[0])
    iv = {c: [] for c in FROM_JOURNAL}
    on = {}		# col -> since
    devs = {}		# usb path -> since
    END = 4e9

    def stop(col, t):
        if col in on:
            iv[col].append((on.pop(col), t, 1))

    def dev_gone(path, t):
        if path in devs:
            iv["usb_dev_s"].append((devs.pop(path), t, 1))
            if not devs:
                stop("usb_host_s", t)

    for t, s in ev:
        if "Linux version" in s:
            for c in list(on):
                stop(c, t)
            for d in list(devs):
                dev_gone(d, t)
        elif "OTG: boost on" in s:
            on.setdefault("otg_s", t)
        elif "OTG: boost off" in s:
            stop("otg_s", t)
        elif ("usbdrd" in s and "device mode" in s) or "usb1-port1: unable to enumerate" in s:
            for d in list(devs):
                dev_gone(d, t)
        elif "exynos9810-dp" in s and "pma: on" in s:
            on.setdefault("dp_link_s", t)
        elif "exynos9810-dp" in s and "drm: enable" in s:
            on.setdefault("dp_video_s", t)
        elif "exynos9810-dp" in s and "drm: disable" in s:
            stop("dp_video_s", t)
        else:
            w = s.split()
            # "usb 1-1.4: new high-speed USB device number 4 using xhci-hcd"
            # "r8152-cfgselector 1-1.4: USB disconnect, device number 4"
            if len(w) >= 3 and w[1].startswith("1-") and w[1].endswith(":") and ":" not in w[1][:-1]:
                path = w[1][:-1]
                if w[2] == "new" and "USB device" in s:
                    devs.setdefault(path, t)
                    on.setdefault("usb_host_s", t)
                elif "USB disconnect" in s:
                    for d in [d for d in devs if d == path or d.startswith(path + ".")]:
                        dev_gone(d, t)
    for c in list(on):
        stop(c, END)
    for d in list(devs):
        dev_gone(d, END)
    return iv


def overlap(ivs, a, b):
    return sum(w * max(0.0, min(b, y) - max(a, x)) for x, y, w in ivs)


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def wifi_idle_j(v, wifi_on, wifi_off):
    on = min(v["screen_s"], v["wifi_assoc_s"])
    return (wifi_on * on + wifi_off * (v["wifi_assoc_s"] - on)) / 1000


def load_profile(path, into):
    for line in open(path):
        p = line.split()
        if len(p) == 2 and not line.startswith("#"):
            try:
                into[p[0]] = float(p[1])
            except ValueError:
                pass


def main():
    args = sys.argv[1:]
    if not args or args[0].startswith("-"):
        print(__doc__)
        return 2
    wifi_on, wifi_off = 315.0, 65.0
    if "--wifi-on" in args:
        wifi_on = float(args[args.index("--wifi-on") + 1])
    if "--wifi-off" in args:
        wifi_off = float(args[args.index("--wifi-off") + 1])
    start = dict(DEFAULTS)
    if "--profile" in args:
        load_profile(args[args.index("--profile") + 1], start)
    fixed = {}
    for i, a in enumerate(args):
        if a == "--fix" and i + 1 < len(args):
            k, v = args[i + 1].split("=")
            fixed[k] = float(v)
    only = None
    if "--only" in args:
        only = args[args.index("--only") + 1].split(",")
        for k, _, _ in COLS:
            if k not in only and k not in fixed:
                fixed[k] = start[k]
    iv = journal_states(args[args.index("--journal") + 1]) if "--journal" in args else None
    rows = list(csv.DictReader(open(args[0])))
    data = []
    filled = 0
    for r in rows:
        v = {k: num(x) for k, x in r.items() if k}
        if not v.get("measured_j") or v["measured_j"] <= 0 or (v.get("boot_s") or 0) < 120:
            continue
        v["cpu"] = v["cpu_little_j"] + v["cpu_big_j"]
        for c in STATE:
            v.setdefault(c, None)
        if iv is not None and v["otg_s"] is None:
            a, b = v["start"], v["start"] + v["boot_s"]
            for c in FROM_JOURNAL:
                v[c] = overlap(iv[c], a, b)
            filled += 1
        data.append(v)
    print(f"{len(data)} windows" + (f", {filled} with states from the journal" if iv is not None else ""))

    holdout = float(args[args.index("--holdout-after") + 1]) if "--holdout-after" in args else None
    train = [v for v in data if holdout is None or v["start"] < holdout]
    test = [v for v in data if holdout is not None and v["start"] >= holdout]
    fit, cols = do_fit(train, start, fixed, wifi_on, wifi_off)
    if holdout is not None:
        print(f"fitted on the {len(train)} windows before {holdout:.0f}, tested on the {len(test)} after")
    for name, _, _ in COLS:
        print(f"{name} {fit[name]:.3f}" + ("" if name in cols else "   # held"))
    if fit["base_awake_mw"] > BASE_MEASURED:
        print(f"# as ztacctd takes it: base_awake_mw {BASE_MEASURED} (measured) + base_extra_mw "
              f"{fit['base_awake_mw'] - BASE_MEASURED:.0f} (Always-on, unseparated)")
    if "--report" in args:
        report(test if holdout is not None else data, fit, wifi_on, wifi_off, "--windows" in args)
    return 0


def target(v, fixed, start, wifi_on, wifi_off):
    y = v["measured_j"] - wifi_idle_j(v, wifi_on, wifi_off) - v["wifi_bytes"] * start["wifi_byte_uj"] / 1e6
    for name, col, scale in COLS:
        if name in fixed and v.get(col) is not None:
            y -= fixed[name] / scale * v[col] if name not in ("cpu_scale", "gpu_scale") else fixed[name] * v[col]
    return y


def do_fit(data, start, fixed0, wifi_on, wifi_off):
    """Least squares, >= 0: the fitted profile and the keys it fitted."""
    fixed = dict(fixed0)
    cols = [c for c in COLS if c[0] not in fixed and any((v.get(c[1]) or 0) > 0 for v in data)]
    for c in [c for c in cols if sum(v.get(c[1]) is not None for v in data) < MIN_ROWS]:
        print(f"  {c[0]}: fewer than {MIN_ROWS} windows know {c[1]}, held")
        fixed[c[0]] = start[c[0]]
        cols.remove(c)
    x = []
    while cols:
        # a row takes part only if it knows every fitted column
        use = [v for v in data if all(v.get(c[1]) is not None for c in cols)]
        ys = [target(v, fixed, start, wifi_on, wifi_off) for v in use]
        a = [[sum(v[ci[1]] * v[cj[1]] for v in use) for cj in cols] for ci in cols]
        b = [sum(v[ci[1]] * y for v, y in zip(use, ys)) for ci in cols]
        x = solve(a, b)
        if x is None:
            print(f"  {cols[-1][0]}: cannot be told apart from the rest, held")
            fixed[cols[-1][0]] = start[cols[-1][0]]
            cols.pop()
            continue
        neg = [i for i, k in enumerate(x) if k < 0]
        if not neg:
            res = [y - sum(k * v[c[1]] for k, c in zip(x, cols)) for v, y in zip(use, ys)]
            tot = sum(v["measured_j"] for v in use)
            print(f"{len(use)} windows; residual {sum(abs(e) for e in res) / max(1e-9, tot) * 100:.1f}% "
                  f"of the measured joules (mean abs)")
            break
        i = min(neg, key=lambda i: x[i])
        print(f"  {cols[i][0]}: fits below 0 ({x[i] * cols[i][2]:.3f}), held at 0")
        fixed[cols[i][0]] = 0
        cols.pop(i)
    fit = dict(start)
    fit.update(fixed)
    for k, c in zip(x, cols):
        fit[c[0]] = k * c[2] if c[0] not in ("cpu_scale", "gpu_scale") else k
    return fit, [c[0] for c in cols]


def parts(v, k, wifi_on, wifi_off):
    """A window's modelled joules by ztacctd's groups (before settling)."""
    def g(col):
        return v.get(col) or 0
    base = min(k["base_awake_mw"], BASE_MEASURED)
    return {
        "Idle (awake) + radios in the base": base * g("awake_s") / 1000 + k["base_suspend_mw"] * g("asleep_s") / 1000
        + g("resumes") * 0.3,
        "Always-on (unseparated)": max(0, k["base_awake_mw"] - BASE_MEASURED) * g("awake_s") / 1000,
        "screen": (k["screen_mw"] * g("screen_s") + k["screen_full_mw"] * g("screen_bright_s")) / 1000,
        "CPU": g("cpu") * k["cpu_scale"],
        "GPU": g("gpu_j") * k["gpu_scale"],
        "Wi-Fi (idle and traffic)": wifi_idle_j(v, wifi_on, wifi_off) + g("wifi_packets") * k["wifi_pkt_mj"] / 1000
        + g("wifi_bytes") * k["wifi_byte_uj"] / 1e6,
        "USB-C accessories": (k["otg_mw"] * g("otg_s") + k["usb_host_mw"] * g("usb_host_s")
                              + k["usb_dev_mw"] * g("usb_dev_s")) / 1000,
        "Desktop (external display)": (k["dp_link_mw"] * g("dp_link_s") + k["dp_video_mw"] * g("dp_video_s")) / 1000,
        "Bluetooth links": k["bt_conn_mw"] * g("bt_conn_s") / 1000,
        "Wakeups": (k["wake_ctxt_uj"] * g("ctxt") + k["wake_irq_uj"] * g("irqs")) / 1e6,
    }


def report(data, fit, wifi_on, wifi_off, windows):
    """As ztacctd settles a window: a model above the gauge scaled down to it,
    one below leaves the rest Unaccounted."""
    import datetime
    if windows:
        print("\nwindow            measured  old model  unacc(old)  new model  unacc(new)   W meas")
    tm = to = tn = uo = un = 0.0
    groups = {}
    for v in data:
        m = v["measured_j"]
        o = v.get("model_j") or 0
        pr = parts(v, fit, wifi_on, wifi_off)
        n = sum(pr.values())
        kk = m / n if n > m else 1
        for name, j in pr.items():
            groups[name] = groups.get(name, 0) + j * kk
        if windows:
            when = datetime.datetime.fromtimestamp(v["start"]).strftime("%m-%d %H:%M")
            print(f"{when}   {m:8.0f}   {o:8.0f}    {max(0, m - o):8.0f}   {n:8.0f}    {max(0, m - n):8.0f}"
                  f"  {m / v['boot_s']:6.2f}")
        tm += m
        to += o
        tn += n
        uo += max(0, m - o)
        un += max(0, m - n)
    if not tm:
        print("no windows")
        return
    print(f"\n{len(data)} windows: measured {tm / 3600:.3f} Wh")
    print(f"  old model {to / 3600:.3f} Wh, Unaccounted {uo / 3600:.3f} Wh ({100 * uo / tm:.1f}%)")
    print(f"  new model {tn / 3600:.3f} Wh, Unaccounted {un / 3600:.3f} Wh ({100 * un / tm:.1f}%)")
    print("  where the measured energy goes, new model:")
    groups["Unaccounted"] = un
    for name, j in sorted(groups.items(), key=lambda x: -x[1]):
        print(f"    {name:36s} {j / 3600:7.3f} Wh  {100 * j / tm:5.1f}%")


if __name__ == "__main__":
    sys.exit(main())
