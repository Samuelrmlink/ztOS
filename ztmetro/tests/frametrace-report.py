#!/usr/bin/env python3
"""Summarise a ztMETRO frame trace ("frametrace MS" on the shell's ctl pipe).

  frametrace-report.py /tmp/zt-frametrace.log [samples.txt|-] [FROM_MS TO_MS]

The trace has one line per frame the shell drew:

  frame END_MS DRAW FINISH COMMIT QUADS UPLOADS SCROLL NEXT [AT_MS]

(END_MS on CLOCK_MONOTONIC; DRAW is the CPU building the frame, FINISH the
glFinish wait for the GPU, COMMIT the atomic commit call; NEXT is 1 while an
animation asked for the next frame, 2 while a finger or a settle is still
moving things, 0 at rest; AT_MS the frame clock's time, since 8.68), and one per page flip the kernel reported:

  pres WHEN_MS SEEN_MS SEQ

Frames in motion (NEXT 1 or 2, and the frame after the last such one) form
runs; within a run the gap between two flips is the frame time the eye sees.
A gap of more than 1.5 refreshes is a missed vblank. With the zrnperfd
sampler's file (shell-perf-sampler.py), each run also gets the clocks and the
power limit level that were in force.
"""
import sys
import statistics as st

REFRESH = 1000.0 / 60


def pct(v, p):
    if not v:
        return float('nan')
    v = sorted(v)
    k = (len(v) - 1) * p / 100
    lo = int(k)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def load(path):
    frames, pres = [], []
    for line in open(path, errors='replace'):
        f = line.split()
        if not f:
            continue
        try:
            if f[0] == 'frame' and len(f) >= 9:
                frames.append(dict(t=float(f[1]), draw=float(f[2]), fin=float(f[3]), com=float(f[4]),
                                   quads=int(f[5]), up=int(f[6]), next=int(f[8]),
                                   start=float(f[1]) - float(f[2]) - float(f[3]) - float(f[4]),
                                   at=float(f[9]) if len(f) >= 10 else None))
            elif f[0] == 'pres' and len(f) >= 4:
                pres.append(float(f[1]))
        except ValueError:
            pass
    return frames, pres


def runs_of(frames):
    runs, cur = [], []
    for fr in frames:
        if fr['next']:
            cur.append(fr)
        elif cur:
            cur.append(fr)	# the settle's last frame belongs to it
            runs.append(cur)
            cur = []
    if cur:
        runs.append(cur)
    return [r for r in runs if len(r) >= 4]


def samples_in(samples, t0, t1):
    return [s for s in samples if t0 - 100 <= s['t'] <= t1 + 100]


def load_samples(path):
    out = []
    lines = open(path).read().split('\n')
    head = lines[0].split()
    for line in lines[1:]:
        f = line.split()
        if len(f) != len(head):
            continue
        d = dict(zip(head, f))
        try:
            d['t'] = float(d['t'])
        except ValueError:
            continue
        out.append(d)
    return out


def main():
    frames, pres = load(sys.argv[1])
    if len(sys.argv) > 4:
        t0, t1 = float(sys.argv[3]), float(sys.argv[4])
        frames = [f for f in frames if t0 <= f['t'] <= t1]
    samples = load_samples(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2] != '-' else []
    runs = runs_of(frames)
    gaps, flips, draws, fins, coms, missed, n_int = [], [], [], [], [], 0, 0
    worst = []
    where = {'first 3': 0, 'middle': 0, 'last 3': 0}
    for r in runs:
        t0, t1 = r[0]['t'], r[-1]['t']
        for a, b in zip(r, r[1:]):
            gaps.append(b['t'] - a['t'])
        p = [x for x in pres if t0 - 1 <= x <= t1 + 40]
        for a, b in zip(p, p[1:]):
            g = b - a
            flips.append(g)
            n_int += 1
            if g > 1.5 * REFRESH:
                missed += round(g / REFRESH) - 1
        draws += [x['draw'] for x in r]
        fins += [x['fin'] for x in r]
        coms += [x['com'] for x in r]
        span = t1 - t0
        fps = (len(p) - 1) / span * 1000 if span > 0 and len(p) > 1 else 0
        info = ''
        ss = samples_in(samples, t0, t1)
        if ss:
            def rng(k, div):
                v = sorted({int(s[k]) // div for s in ss if s.get(k, '-').lstrip('-').isdigit()})
                return '%s' % (v[0] if len(v) == 1 else '%d-%d' % (v[0], v[-1])) if v else '?'
            pl = sorted({s.get('plevel', '-') for s in ss})
            ma = [abs(int(s['ma'])) // 1000 for s in ss if s.get('ma', '-').lstrip('-').isdigit()]
            info = '  little %s MHz (max %s) big %s gpu %s MHz (max %s) plevel %s %s mA' % (
                rng('l', 1000), rng('lmax', 1000), rng('b', 1000), rng('g', 1000000),
                rng('gmax', 1000000), ','.join(pl), int(st.mean(ma)) if ma else '?')
        pg = [b - a for a, b in zip(p, p[1:])]
        for i, g in enumerate(pg):
            if g > 1.5 * REFRESH:
                where['first 3' if i < 3 else 'last 3' if i >= len(pg) - 3 else 'middle'] += 1
        worst.append((max(pg) if pg else 0, t0))
        print('run at %.0f: %3d frames %5.0f ms, %4.1f fps, flip gap p50 %.1f max %.1f, draw %.1f fin %.1f com %.1f%s' % (
            t0, len(r), span, fps, pct(pg, 50), max(pg) if pg else 0, st.mean(x['draw'] for x in r),
            st.mean(x['fin'] for x in r), st.mean(x['com'] for x in r), info))
    print()
    print('%d runs, %d frames in motion' % (len(runs), sum(len(r) for r in runs)))
    for name, v in (('shell frame interval', gaps), ('flip interval (presented)', flips),
                    ('draw (CPU)', draws), ('glFinish (GPU wait)', fins), ('commit call', coms)):
        if v:
            print('%-26s p50 %6.2f  p95 %6.2f  p99 %6.2f  max %6.1f ms  (n=%d)' % (
                name, pct(v, 50), pct(v, 95), pct(v, 99), max(v), len(v)))
    if flips:
        print('achieved rate in motion: %.1f Hz (mean flip interval %.2f ms)' % (1000 / st.mean(flips), st.mean(flips)))
        print('missed vblanks: %d over %d flips (%.1f %%)' % (missed, n_int, 100.0 * missed / max(1, n_int + missed)))
        late = sum(1 for g in flips if g > 1.5 * REFRESH)
        print('where the late flips fall in a run: %s' % where)
        print('flips later than 1.5 refreshes: %d (%.1f %%)' % (late, 100.0 * late / len(flips)))
    early, rest = clock_steps(runs)
    for name, v in (('clock step error, first 10', early), ('clock step error, after', rest)):
        if v:
            print('%-26s p50 %6.2f  p90 %6.2f  p99 %6.2f  max %6.1f ms  (n=%d)' % (
                name, pct(v, 50), pct(v, 90), pct(v, 99), max(v), len(v)))


def clock_steps(runs):
    """How unevenly the animations' clock stepped between frames one flip
    apart: the frame clock's time when the trace has it, else the frame's
    start (the clock before 8.68). First 10 frames of a run, then the rest."""
    early, rest = [], []
    for r in runs:
        key = 'at' if r[0]['at'] is not None else 'start'
        for i, (a, b) in enumerate(zip(r, r[1:])):
            if a['next'] != 1:
                continue
            d = b[key] - a[key]
            if d > 1.5 * REFRESH:
                continue	# a missed flip, not jitter
            (early if i < 10 else rest).append(abs(d - REFRESH))
    return early, rest


if __name__ == '__main__':
    main()
