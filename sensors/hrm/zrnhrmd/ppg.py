"""ppg: heart rate and SpO2 from raw IR and red PPG samples (pure Python).

Heart rate:
- Each channel is band-passed: a 1 s moving average is subtracted (high
  pass), then a 5-tap moving average smooths it (low pass, about 15 Hz at
  100 Hz).
- The pulse signal is inverted (more blood absorbs more light).
- Beats are found with Elgendi's two moving averages (Elgendi et al. 2013,
  PLoS ONE 8(10): e76585): a short window (111 ms) over the squared
  positive signal, compared with a long one (667 ms). The peak is the
  highest sample in each block where the short one is above the long one.
- The rate is 60 / median beat interval.

SpO2 uses the "ratio of ratios", R = (AC_red / DC_red) / (AC_ir / DC_ir), with
AC as the RMS of the band-passed signal and DC as the window mean.
SPO2 = A - B * R. The default A = 110, B = 25 is Maxim's application-note
line. It is NOT calibrated for the S9: a reading is a wellness estimate, not
a medical measurement.
"""
from math import sqrt

RATE = 100.0                # sets per second (zrn-hrm: SR 400 Hz, average 4)
SPO2_A, SPO2_B = 110.0, 25.0


def _mavg(x, n):
    """Centred moving average, window n (edges use what there is)."""
    out, s, h = [], 0.0, n // 2
    pre = [0.0]
    for v in x:
        s += v
        pre.append(s)
    for i in range(len(x)):
        a, b = max(0, i - h), min(len(x), i + h + 1)
        out.append((pre[b] - pre[a]) / (b - a))
    return out


def bandpass(x, rate=RATE):
    hp = [v - m for v, m in zip(x, _mavg(x, int(rate)))]
    return _mavg(hp, 5)


def beats(sig, rate=RATE):
    """Indices of pulse peaks in a band-passed, inverted signal."""
    y = [v * v if v > 0 else 0.0 for v in sig]
    peak = _mavg(y, max(3, int(0.111 * rate)))
    beat = _mavg(y, max(5, int(0.667 * rate)))
    mean_y = sum(y) / len(y) if y else 0.0
    alpha = 0.02 * mean_y
    out, i, n = [], 0, len(sig)
    min_w = int(0.111 * rate)
    while i < n:
        if peak[i] > beat[i] + alpha:
            j = i
            while j < n and peak[j] > beat[j] + alpha:
                j += 1
            if j - i >= min_w:
                k = max(range(i, j), key=lambda q: sig[q])
                if not out or k - out[-1] > 0.3 * rate:     # 200 bpm limit
                    out.append(k)
            i = j
        else:
            i += 1
    return out


def heart_rate(ir, rate=RATE):
    """(bpm, quality 0..1, beat indices) or (None, 0, []) from IR samples."""
    if len(ir) < 4 * rate:
        return None, 0.0, []
    sig = [-v for v in bandpass(ir, rate)]
    pk = beats(sig, rate)
    ibi = [(b - a) / rate for a, b in zip(pk, pk[1:])]
    ibi = [d for d in ibi if 0.3 <= d <= 2.0]          # 30..200 bpm
    if len(ibi) < 4:
        return None, 0.0, pk
    med = sorted(ibi)[len(ibi) // 2]
    good = sum(1 for d in ibi if abs(d - med) <= 0.2 * med) / len(ibi)
    return 60.0 / med, good, pk


def _ac_dc(x, rate):
    dc = sum(x) / len(x)
    bp = bandpass(x, rate)
    ac = sqrt(sum(v * v for v in bp) / len(bp))
    return ac, dc


def spo2(ir, red, rate=RATE, a=SPO2_A, b=SPO2_B):
    """(spo2 %, R, perfusion index % of IR) or (None, R, pi) when gated out."""
    ac_ir, dc_ir = _ac_dc(ir, rate)
    ac_red, dc_red = _ac_dc(red, rate)
    if dc_ir <= 0 or dc_red <= 0 or ac_ir <= 0:
        return None, None, 0.0
    pi = 100.0 * ac_ir / dc_ir
    r = (ac_red / dc_red) / (ac_ir / dc_ir)
    if not 0.05 <= pi <= 20.0 or not 0.2 <= r <= 2.0:
        return None, r, pi
    return max(0.0, min(100.0, a - b * r)), r, pi


def finger_on(ir, threshold):
    """A finger reflects most of the IR back: DC well above the bare-glass level."""
    return bool(ir) and sum(ir) / len(ir) >= threshold
