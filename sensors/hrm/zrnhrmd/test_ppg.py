"""Host tests for ppg.py with synthetic PPG (no phone needed).

    python3 -I sensors/hrm/zrnhrmd/test_ppg.py
"""
import math
import os
import random
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppg  # noqa: E402


def synth(bpm, seconds, r=0.6, pi=1.5, noise=0.002, drift=0.02, seed=1, rate=100.0):
    """IR and red with a pulse of the given rate, a known R and perfusion index."""
    rnd = random.Random(seed)
    ir, red = [], []
    dc_ir, dc_red = 200000.0, 150000.0
    f = bpm / 60.0
    for i in range(int(seconds * rate)):
        t = i / rate
        # a systolic upstroke and a dicrotic bump; more blood = less light
        ph = (t * f) % 1.0
        pulse = math.exp(-((ph - 0.15) / 0.07) ** 2) + 0.35 * math.exp(-((ph - 0.45) / 0.08) ** 2)
        base = 1.0 + drift * math.sin(2 * math.pi * 0.2 * t)       # breathing
        a_ir = pi / 100.0 * 1.9 * pulse
        a_red = a_ir * r
        ir.append(dc_ir * base * (1 - a_ir) * (1 + rnd.gauss(0, noise)))
        red.append(dc_red * base * (1 - a_red) * (1 + rnd.gauss(0, noise)))
    return ir, red


class HeartRate(unittest.TestCase):
    def test_rates(self):
        for bpm in (48, 60, 72, 95, 130, 170):
            ir, _ = synth(bpm, 10, seed=bpm)
            got, quality, _ = ppg.heart_rate(ir)
            self.assertIsNotNone(got, bpm)
            self.assertAlmostEqual(got, bpm, delta=max(2.0, 0.03 * bpm), msg=f"{bpm}: {got}")
            self.assertGreater(quality, 0.8)

    def test_too_short(self):
        ir, _ = synth(72, 2)
        self.assertEqual(ppg.heart_rate(ir)[0], None)

    def test_flat_has_no_rate(self):
        rnd = random.Random(3)
        ir = [200000 * (1 + rnd.gauss(0, 0.002)) for _ in range(1000)]
        bpm, quality, _ = ppg.heart_rate(ir)
        self.assertTrue(bpm is None or quality < 0.6)


class SpO2(unittest.TestCase):
    def test_ratio_recovered(self):
        for r in (0.5, 0.6, 0.8, 1.0):
            ir, red = synth(75, 8, r=r, seed=int(r * 10))
            s, got_r, pi = ppg.spo2(ir, red)
            self.assertAlmostEqual(got_r, r, delta=0.05)
            self.assertAlmostEqual(s, 110 - 25 * got_r, delta=0.01)
            self.assertGreater(pi, 0.5)

    def test_no_pulse_gated(self):
        ir = [200000.0] * 800
        red = [150000.0] * 800
        self.assertIsNone(ppg.spo2(ir, red)[0])

    def test_finger(self):
        self.assertTrue(ppg.finger_on([60000] * 10, 50000))
        self.assertFalse(ppg.finger_on([3000] * 10, 50000))


class DaemonReplay(unittest.TestCase):
    """zrn-hrmd.py --replay on a file of zrn-hrm records, into a temp dir."""

    def test_replay(self):
        import struct
        import subprocess
        import tempfile
        rec = struct.Struct("<QIIII")
        ir, red = synth(66, 10, r=0.55)
        with tempfile.TemporaryDirectory() as d:
            raw = os.path.join(d, "rec.bin")
            with open(raw, "wb") as f:
                for i, (a, b) in enumerate(zip(ir, red)):
                    f.write(rec.pack(i * 10_000_000, i, int(a), int(b), 0))
            env = dict(os.environ, ZRN_HRM_OUT=d)
            daemon = os.path.join(os.path.dirname(os.path.abspath(__file__)), "zrn-hrmd.py")
            out = subprocess.run([sys.executable, "-I", daemon, "--replay", raw, "--seconds", "30"],
                                 env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(out.returncode, 0, out.stderr)
            with open(os.path.join(d, "hrm")) as f:
                kv = dict(line.split("=", 1) for line in f.read().split())
            self.assertEqual(kv["state"], "done")
            self.assertAlmostEqual(int(kv["bpm"]), 66, delta=2)
            self.assertAlmostEqual(int(kv["spo2"]), round(110 - 25 * 0.55), delta=2)
            self.assertTrue(os.path.getsize(os.path.join(d, "hrm-wave")) > 0)


if __name__ == "__main__":
    unittest.main()
