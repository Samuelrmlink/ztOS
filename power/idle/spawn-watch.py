#!/usr/bin/env python3
"""Which processes keep starting short-lived children (ROADMAP 8.48).

    ssh ... samuel@ztos 'sudo python3 - [SECONDS]' < power/idle/spawn-watch.py

Polls /proc every 20 ms for SECONDS (default 15) and counts new processes
by parent and command line. Every exec costs a CPU wake-up and a page-cache
walk; on an idle phone the counts should be near zero. Reads /proc only.
"""
import collections
import os
import sys
import time

T = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
seen = set(p for p in os.listdir('/proc') if p.isdigit())
count = collections.Counter()
t0 = time.time()
while time.time() - t0 < T:
    now = set(p for p in os.listdir('/proc') if p.isdigit())
    for p in now - seen:
        try:
            cmd = open('/proc/%s/cmdline' % p, 'rb').read().replace(b'\0', b' ').decode(errors='replace')[:100]
            ppid = open('/proc/%s/stat' % p).read().rsplit(') ', 1)[1].split()[1]
            parent = open('/proc/%s/comm' % ppid).read().strip()
        except (OSError, IndexError):
            continue
        count[(parent, cmd)] += 1
    seen |= now
    time.sleep(0.02)
print('new processes in %.0f s:' % T)
for (parent, cmd), n in count.most_common(20):
    print('%4d  %-16s -> %s' % (n, parent, cmd))
