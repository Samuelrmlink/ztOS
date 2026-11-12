#!/usr/bin/env python3
"""The panel clock's hands (ROADMAP 8.45, power/aod/PANEL-AOD.md 1.2).

  make-sprite.py OUT.bin [--png OUT.png] [--grey A0]

The S6E3HA8's analogue live clock turns three hands from one 480 x 120 RGBA
image in its side RAM: 40-row bands for the hour, the minute and the second
hand, each drawn horizontal, pointing right (stock's own image, decoded from
s6e3ha8_analog_clock_img.h: hands from x 102 to 324 / 412 / 413, centred on
the band's row 19-20, transparent FF FF FF 00 around them).

Ours keeps stock's geometry: each hand starts where stock's do (just before
x 120), so it turns about the same point, and stays inside the lengths stock
uses. The hands are thin and dim: 3 px, grey, anti-aliased by alpha. No
second hand: its band is left transparent.
"""
import argparse
import struct
import zlib

W, H, BAND = 480, 120, 40
ap = argparse.ArgumentParser()
ap.add_argument('out')
ap.add_argument('--png')
ap.add_argument('--grey', default='A0')
a = ap.parse_args()
g = int(a.grey, 16)
px = [[(0xFF, 0xFF, 0xFF, 0x00)] * W for _ in range(H)]


def hand(band, x0, x1, half=1.5):
	cy = band * BAND + 19.5
	for y in range(band * BAND, (band + 1) * BAND):
		for x in range(x0 - 2, x1 + 3):
			# distance to the segment (x0..x1, cy), round ends
			dx = max(x0 - x, 0, x - x1)
			d = (dx * dx + (y - cy) ** 2) ** 0.5
			cov = max(0.0, min(1.0, half + 0.5 - d))
			if cov > 0:
				px[y][x] = (g, g, g, round(255 * cov))


hand(0, 110, 300)	# hour: stock's runs 102-324
hand(1, 110, 400)	# minute: stock's runs 102-412
# band 2 (second hand): none
data = b''.join(bytes(p) for row in px for p in row)
assert len(data) == 230400
open(a.out, 'wb').write(data)
if a.png:
	raw = b''.join(b'\x00' + bytes(p[3] for p in row) for row in px)
	def chunk(t, d):
		return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
	open(a.png, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 0, 0, 0, 0)) +
				chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
print(f'{a.out}: 230400 bytes, grey {g:02X}')
