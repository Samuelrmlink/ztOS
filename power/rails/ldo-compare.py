#!/usr/bin/env python3
"""Compare the S2MPS18 LDO enable modes: stock against ztOS now.

    ssh ... samuel@ztos 'cat /sys/class/sec/sensorhub/ldo' > ldo-now.txt
    python3 -I power/rails/stock-regulators.py dtb11.txt > regs.tsv
    python3 -I power/rails/ldo-compare.py ldo-now.txt regs.tsv

The `ldo` file belongs to zrn-bbd and reads LDO1-45 through ACPM (read
only). The stock column is sensors/stock-state/s2mps18-regs.txt (stock
kernel, TWRP); `ml` is sensors/stock-state/s2mps18-regs-mainline.txt (an
early ztOS boot). Bits 7:6 of LnCTRL: 0 off, 1 on with PWREN (off in
sleep), 2 on with PWREN_MIF (off when MIF is down), 3 always on.
Rows marked * differ in mode from stock. Reads files only.
"""
import re
import sys

st = {int(a, 16): int(b, 16) for a, b in
      (l.split() for l in open('sensors/stock-state/s2mps18-regs.txt'))}
ml = {int(l[:2], 16): int(l[4:6], 16)
      for l in open('sensors/stock-state/s2mps18-regs-mainline.txt')}
now = {}
for l in open(sys.argv[1]):
    m = re.match(r'LDO(\d+) ([0-9a-f]{2})', l)
    if m:
        now[int(m.group(1))] = int(m.group(2), 16)
names = {}
for l in open(sys.argv[2]):
    f = l.rstrip('\n').split('\t')
    m = re.search(r'/s2mps18mfd@00/regulators/LDO(\d+)$', f[0])
    if m:
        names[int(m.group(1))] = (f[1], f[2], f[3], f[4])


def reg(n):
    return 0x49 if n == 1 else 0x4a if n == 2 else 0x49 + n


for n in range(1, 46):
    r = reg(n)
    nm = names.get(n, ('(not in DT)', '-', '-', '-'))
    s, m_, c = st[r], ml[r], now.get(n, 0)
    print('L%-2d 0x%02x %-19s %-11s stock %02x m%d | early %02x m%d | now %02x m%d %s| %s | %s'
          % (n, r, nm[0], nm[1], s, s >> 6, m_, m_ >> 6, c, c >> 6,
             '* ' if (s >> 6) != (c >> 6) else '', nm[2], nm[3]))
