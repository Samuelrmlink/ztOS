#!/usr/bin/env python3
"""Pull Samsung's device trees out of stock/BOOT.img and print them as text.

    python3 -I camera/iris/stock-dt.py stock/BOOT.img OUTDIR

Writes OUTDIR/dtbN_OFFSET.dtb and .txt for every flattened tree in the image
(twelve on this phone: Star rev16 to rev26, twice). The text is close to dtc
-I dtb -O dts, with phandles left as numbers; `--phandle FILE.txt 0x4d ...`
names the node each phandle points at. Reads only; needs no dtc.
"""
import os
import re
import struct
import sys


def fdt_to_text(d):
    _, _, off_st, off_str, _, _, _, _, szstr, _ = struct.unpack('>10I', d[:40])
    strs = d[off_str:off_str + szstr]

    def name(o):
        return strs[o:strs.index(b'\0', o)].decode()

    def fmt(v):
        if (v and v[-1] == 0 and v[0] != 0 and b'\0\0' not in v
                and all(32 <= c < 127 or c == 0 for c in v[:-1])):
            return '"' + '","'.join(x.decode() for x in v[:-1].split(b'\0')) + '"'
        if len(v) % 4 == 0:
            return '<' + ' '.join('0x%x' % x for x in struct.unpack('>%dI' % (len(v) // 4), v)) + '>'
        return '[' + v.hex() + ']'

    p, depth, out = off_st, 0, []
    while True:
        t = struct.unpack('>I', d[p:p + 4])[0]
        p += 4
        if t == 1:
            e = d.index(b'\0', p)
            out.append('  ' * depth + (d[p:e].decode() or '/') + ' {')
            p = (e + 4) & ~3
            depth += 1
        elif t == 2:
            depth -= 1
            out.append('  ' * depth + '};')
        elif t == 3:
            ln, no = struct.unpack('>II', d[p:p + 8])
            p += 8
            v = d[p:p + ln]
            p = (p + ln + 3) & ~3
            out.append('  ' * depth + name(no) + (' = ' + fmt(v) if ln else '') + ';')
        elif t == 9:
            return '\n'.join(out) + '\n'


def phandles(txt):
    stack, ph = [], {}
    for line in txt.splitlines():
        s = line.strip()
        if s.endswith(' {'):
            stack.append(s[:-2])
        elif s == '};':
            stack.pop()
        else:
            m = re.match(r'phandle = <(0x[0-9a-f]+)>;', s)
            if m:
                ph[int(m.group(1), 16)] = '/' + '/'.join(stack[1:])
    return ph


def main():
    if len(sys.argv) > 2 and sys.argv[1] == '--phandle':
        ph = phandles(open(sys.argv[2]).read())
        for a in sys.argv[3:]:
            print(a, ph.get(int(a, 16)))
        return
    img, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    d = open(img, 'rb').read()
    i = n = 0
    while (i := d.find(b'\xd0\x0d\xfe\xed', i)) >= 0:
        size = struct.unpack('>I', d[i + 4:i + 8])[0]
        if 0x1000 < size < 0x400000:
            base = os.path.join(out, 'dtb%d_%x' % (n, i))
            blob = d[i:i + size]
            open(base + '.dtb', 'wb').write(blob)
            txt = fdt_to_text(blob)
            open(base + '.txt', 'w').write(txt)
            model = re.search(r'^  model = "([^"]*)"', txt, re.M)
            print(base, model.group(1) if model else '?')
            n += 1
        i += 4


if __name__ == '__main__':
    main()
