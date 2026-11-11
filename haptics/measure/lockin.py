#!/usr/bin/env python3
"""lockin.py WAV [F] : amplitude and phase of the F Hz (159.9) component, per
1 ms, around each pulse; phase relative to the pulse's own peak. Shows whether
the drive after a pulse (the brake) opposes the motion (amplitude falls, phase
kept) or adds to it."""
import cmath, math, sys, wave, struct

w = wave.open(sys.argv[1]); fs = w.getframerate(); ch = w.getnchannels()
F = float(sys.argv[2]) if len(sys.argv) > 2 else 159.9
raw = w.readframes(w.getnframes())
v = struct.unpack('<%dh' % (len(raw) // 2), raw)
x = [float(v[i]) for i in range(0, len(v), ch)]
N = int(round(fs / F * 2))  # two periods
ref = [cmath.exp(-2j * math.pi * F * i / fs) for i in range(len(x))]
prod = [a * b for a, b in zip(x, ref)]
cum = [0j]
for p in prod: cum.append(cum[-1] + p)
hop = fs // 1000
z = [(cum[i + N] - cum[i]) / N * 2 for i in range(0, len(x) - N, hop)]
amp = [abs(c) for c in z]
SKIP = 300
floor = sorted(amp[SKIP:])[len(amp[SKIP:]) // 2]
i = SKIP; pulses = []
while i < len(amp):
    if amp[i] > floor * 10:
        pulses.append(i); i += 400
    else:
        i += 1
for n, s in enumerate(pulses):
    seg = amp[s:s + 150]; pk = max(seg); pi = seg.index(pk)
    ph0 = cmath.phase(z[s + pi])
    print('#%d @%d ms peak %.0f at +%d ms (floor %.1f)' % (n, s, pk, pi, floor))
    line = []
    for t in range(-4, 120, 3):
        c = z[s + t]
        d = math.degrees(cmath.phase(c) - ph0)
        d = (d + 180) % 360 - 180
        line.append('%+4d:%3.0f%%%+4.0f' % (t, 100 * abs(c) / pk, d))
    for k in range(0, len(line), 7):
        print('   ' + '  '.join(line[k:k + 7]))
