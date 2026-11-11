#!/usr/bin/env python3
"""ana.py WAV [v] : the motor's buzz in the mic, per pulse.
Band-pass 100..1000 Hz, RMS over one motor period (6 ms) every 1 ms; floor =
median. A pulse starts where the envelope first passes 10x floor. Reported:
peak, rise, last time above -10/-20/-30 dB of the peak, and the mean level
100-300 ms after the start (a residual hum) against the floor."""
import math, sys, wave, struct

def biquad(x, b, a):
    y = [0.0] * len(x); x1 = x2 = y1 = y2 = 0.0
    b0, b1, b2 = b; a1, a2 = a
    for i, v in enumerate(x):
        o = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y[i] = o
    return y

def coef(kind, f, fs, q=0.707):
    w = 2 * math.pi * f / fs; c = math.cos(w); s = math.sin(w) / (2 * q)
    b = [(1 + c) / 2, -(1 + c), (1 + c) / 2] if kind == 'hp' else [(1 - c) / 2, 1 - c, (1 - c) / 2]
    a0 = 1 + s
    return [v / a0 for v in b], [-2 * c / a0, (1 - s) / a0]

w = wave.open(sys.argv[1]); fs = w.getframerate(); ch = w.getnchannels()
raw = w.readframes(w.getnframes())
v = struct.unpack('<%dh' % (len(raw) // 2), raw)
x = [float(v[i]) for i in range(0, len(v), ch)]
for kind, f in (('hp', 100), ('hp', 100), ('lp', 1000), ('lp', 1000)):
    b, a = coef(kind, f, fs); x = biquad(x, b, a)
cum = [0.0]
for s in x: cum.append(cum[-1] + s * s)
hop = fs // 1000; win = fs * 6 // 1000
env = [math.sqrt((cum[i + win] - cum[i]) / win) for i in range(0, len(x) - win, hop)]
SKIP = 300
env = env[SKIP:]
floor = sorted(env)[len(env) // 2] or 1
th = floor * 10
i = 0; pulses = []
while i < len(env):
    if env[i] > th:
        pulses.append(i); i += 450
    else:
        i += 1
print('floor %.1f' % floor)
for n, s in enumerate(pulses):
    seg = env[s:s + 400]
    pk = max(seg); pi = seg.index(pk)
    def last_above(db):
        r = 10 ** (db / 20); idx = 0
        for t, val in enumerate(seg[:300]):
            if val > pk * r: idx = t
        return idx
    pre = env[max(0, s - 120):s - 20]
    pre = sum(pre) / len(pre) if pre else floor
    tail = seg[100:300]; tail = sum(tail) / len(tail) if tail else 0
    print('#%d @%5d ms: peak %6.0f rise %2d ms | -10dB %3d  -20dB %3d  -30dB %3d ms | tail(100-300ms) %.1f vs before %.1f'
          % (n, s + SKIP, pk, pi, last_above(-10), last_above(-20), last_above(-30), tail, pre))
    if len(sys.argv) > 2:
        bar = ''.join(' .:-=+*#%@'[min(9, int(9 * val / pk + 0.5))] for val in env[s - 4:s + 140:2])
        print('   |' + bar + '|')
