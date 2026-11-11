#!/usr/bin/env python3
"""Read-only: MIF PLL and dividers (the seven fields audit-mif-frequency.py matches)."""
import mmap, os, struct
ADDR = [0x1b800120, 0x1a2401a0, 0x1a2402a0, 0x1a2410a4, 0x1a24102c, 0x1a2418a0, 0x1a241828]
fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
out = {}
for a in ADDR:
    with mmap.mmap(fd, 4096, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ, offset=a & ~4095) as m:
        out[a] = struct.unpack_from('<I', m, a & 4095)[0]
os.close(fd)
for a, v in out.items():
    line = f'{a:#010x} = {v:#010x}'
    if a in ADDR[:3]:
        M, P, S = (v >> 16) & 0x3ff, (v >> 8) & 0x3f, v & 7
        en = (v >> 31) & 1
        if P:
            line += f'  enable={en} M={M} P={P} S={S} -> {26 * M / (P * (1 << S)):.1f} MHz'
    print(line)
