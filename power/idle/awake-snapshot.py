#!/usr/bin/env python3
"""Read-only snapshot of what an awake phone is doing (ROADMAP 8.48).

    ssh ... samuel@ztos 'sudo python3 - [SECONDS]' < power/idle/awake-snapshot.py

Over SECONDS (default 30): interrupts per second by source, the busiest
threads (CPU ms per second, context switches per second), cpufreq time in
state, and the state of the things that keep a phone warm. Reads /proc and
/sys only, in one process, so it adds little of its own.
"""
import os
import sys
import time

T = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
HZ = os.sysconf('SC_CLK_TCK')


def rd(p, d=''):
    try:
        with open(p) as f:
            return f.read().strip()
    except OSError:
        return d


def irqs():
    out = {}
    lines = rd('/proc/interrupts').splitlines()
    ncpu = len(lines[0].split())
    for l in lines[1:]:
        f = l.split()
        if not f or not f[0].endswith(':'):
            continue
        n = 0
        for x in f[1:1 + ncpu]:
            if x.isdigit():
                n += int(x)
        out[f[0] + ' ' + (f[-1] if len(f) > ncpu + 1 else '')] = n
    return out


def threads():
    out = {}
    for pid in os.listdir('/proc'):
        if not pid.isdigit():
            continue
        try:
            tids = os.listdir('/proc/%s/task' % pid)
        except OSError:
            continue
        pcomm = rd('/proc/%s/comm' % pid)
        for tid in tids:
            st = rd('/proc/%s/task/%s/stat' % (pid, tid))
            if not st:
                continue
            rest = st[st.rfind(')') + 2:].split()
            cpu = int(rest[11]) + int(rest[12])
            sw = 0
            for l in rd('/proc/%s/task/%s/status' % (pid, tid)).splitlines():
                if l.endswith('ctxt_switches:') or 'ctxt_switches' in l:
                    sw += int(l.split()[-1])
            comm = st[st.find('(') + 1:st.rfind(')')]
            out[(pid, tid)] = (cpu, sw, pcomm, comm)
    return out


def freq():
    out = {}
    base = '/sys/devices/system/cpu/cpufreq'
    for p in sorted(os.listdir(base)):
        out[p] = {}
        for l in rd('%s/%s/stats/time_in_state' % (base, p)).splitlines():
            k, v = l.split()
            out[p][int(k)] = int(v)
    return out


B = '/sys/class/power_supply'
print('== %s up %ss soc=%s%% chg=%s current_now=%s' % (
    time.strftime('%F %T'), rd('/proc/uptime').split()[0], rd(B + '/max170xx_battery/capacity'),
    rd(B + '/max77705-charger/online'), rd(B + '/max170xx_battery/current_now')))
print(' '.join(l for l in rd('/run/zrnperfd.status').splitlines() if l.startswith(('screen', 'mif', 'boost'))))
print('cpuidle driver: %s; online %s' % (rd('/sys/devices/system/cpu/cpuidle/current_driver', 'none'),
                                         rd('/sys/devices/system/cpu/online')))
print('abox idle_off_ms=%s offs=%s; waydroid frozen=%s' % (
    rd('/sys/module/snd_zrn_abox/parameters/idle_off_ms'), rd('/sys/module/snd_zrn_abox/parameters/idle_offs'),
    rd('/sys/fs/cgroup/lxc.payload.waydroid/cgroup.freeze', '-')))
i0, t0, f0, w0 = irqs(), threads(), freq(), time.time()
time.sleep(T)
i1, t1, f1 = irqs(), threads(), freq()
dt = time.time() - w0
print('-- interrupts/s over %.0f s' % dt)
for r, k in sorted(((i1[k] - i0.get(k, 0)) / dt, k) for k in i1)[::-1][:15]:
    if r >= 1:
        print('%8.1f  %s' % (r, k))
print('-- cpufreq: share of time per frequency (MHz)')
for p in f1:
    tot = sum(f1[p][k] - f0[p].get(k, 0) for k in f1[p]) or 1
    print(p, ' '.join('%d:%.0f%%' % (k // 1000, 100.0 * (f1[p][k] - f0[p].get(k, 0)) / tot)
                      for k in sorted(f1[p]) if f1[p][k] - f0[p].get(k, 0) > 0))
print('-- threads: CPU ms/s, switches/s (top 25)')
rows = []
for k, (cpu, sw, pc, c) in t1.items():
    if k in t0:
        rows.append(((cpu - t0[k][0]) * 1000.0 / HZ / dt, (sw - t0[k][1]) / dt, k[0], pc, c))
for cpu, sw, pid, pc, c in sorted(rows, reverse=True)[:25]:
    print('%7.1f ms/s %7.1f sw/s  %6s %-16s %s' % (cpu, sw, pid, pc, c))
print('-- processes: CPU ms/s (top 15)')
per = {}
for cpu, sw, pid, pc, c in rows:
    a = per.setdefault((pid, pc), [0.0, 0.0])
    a[0] += cpu
    a[1] += sw
for (pid, pc), (cpu, sw) in sorted(per.items(), key=lambda x: -x[1][0])[:15]:
    print('%7.1f ms/s %7.1f sw/s  %6s %s' % (cpu, sw, pid, pc))
print('total CPU %.0f ms/s over all threads' % sum(r[0] for r in rows))
