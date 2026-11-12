#!/usr/bin/env python3
"""Turn a PNG by quarter turns counter-clockwise, with no image library
(the dev container and the PC have none): 8-bit RGB or RGBA, as grim writes.

    png-turn.py IN OUT QUARTERS
"""
import struct
import sys
import zlib


def read(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    at, idat = 8, b''
    while at < len(data):
        n, kind = struct.unpack('>I4s', data[at:at + 8])
        body = data[at + 8:at + 8 + n]
        if kind == b'IHDR':
            w, h, depth, colour = struct.unpack('>IIBB', body[:10])
            assert depth == 8 and colour in (2, 6), 'only 8-bit RGB or RGBA'
            bpp = 3 if colour == 2 else 4
        elif kind == b'IDAT':
            idat += body
        at += 12 + n
    raw, stride = zlib.decompress(idat), w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, bpp, colour, rows


def write(path, w, h, colour, rows):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    raw = b''.join(b'\0' + r for r in rows)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, colour, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def turn_ccw(w, h, bpp, rows):
    """one quarter counter-clockwise: the right-hand column becomes the top row"""
    out = []
    for x in range(w - 1, -1, -1):
        out.append(b''.join(rows[y][x * bpp:(x + 1) * bpp] for y in range(h)))
    return h, w, out


def main():
    src, dst, q = sys.argv[1], sys.argv[2], int(sys.argv[3]) % 4
    w, h, bpp, colour, rows = read(src)
    for _ in range(q):
        w, h, rows = turn_ccw(w, h, bpp, rows)
    write(dst, w, h, colour, rows)


if __name__ == '__main__':
    main()
