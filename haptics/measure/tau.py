#!/usr/bin/env python3
"""tau.py WAV... : the motor's ring-down time constant after each pulse (160 Hz lock-in,
one-period window, log-linear fit from 70 % down to 3 % of the peak), and how long
each pulse is above 10 % / 3 % of its peak from its onset."""
import wave, struct, math, cmath, sys

def env(f, F=160.0):
    w = wave.open(f); fs = w.getframerate(); ch = w.getnchannels()
    raw = w.readframes(w.getnframes()); v = struct.unpack('<%dh' % (len(raw) // 2), raw)
    x = [float(s) for s in v[0::ch]]
    N = 300
    cum = [0j]
    for i, s in enumerate(x):
        cum.append(cum[-1] + s * cmath.exp(-2j * math.pi * F * i / fs))
    return [abs(cum[i + N] - cum[i]) / N * 2 for i in range(0, len(x) - N, 48)]

for f in sys.argv[1:]:
    e = env(f)[300:]; top = max(e)
    fl = sorted(e)[len(e) // 2]
    print('%s: floor %.0f (%.1f%% of the loudest)' % (f, fl, 100 * fl / top))
    i = 0
    while i < len(e):
        if e[i] > max(top * 0.1, fl * 8):
            s = i - 3; seg = e[s:s + 300]; pk = max(seg[:150]); p = seg.index(pk)
            run = []
            for t in range(p + 1, len(seg)):
                v = seg[t]
                if 0.03 * pk < v < 0.7 * pk: run.append((t, math.log(v)))
                elif v <= 0.03 * pk: break
            on = next(t for t, v in enumerate(seg) if v > 0.1 * pk)
            last10 = max(t for t, v in enumerate(seg[:250]) if v > 0.1 * pk)
            last3 = max(t for t, v in enumerate(seg[:250]) if v > 0.03 * pk)
            tau = ''
            if len(run) > 5:
                n = len(run); mt = sum(t for t, _ in run) / n; ml = sum(l for _, l in run) / n
                sl = sum((t - mt) * (l - ml) for t, l in run) / sum((t - mt) ** 2 for t, _ in run)
                tau = 'tau %.1f ms' % (-1 / sl)
            print('  @%5d peak %6.0f: above 10%% for %3d ms, above 3%% for %3d ms; %s'
                  % (i + 300, pk, last10 - on, last3 - on, tau))
            i += 450
        else:
            i += 1
