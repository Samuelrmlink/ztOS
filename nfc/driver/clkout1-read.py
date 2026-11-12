#!/usr/bin/env python3
"""Stage 2a (owner-approved 2026-11-12): ONE 32-bit read of PMU CLKOUT1
(0x14060A0C, OSC_NFC). No write. Exit 0 = enabled (bit 0 clear),
3 = gated (bit 0 set), other = error.

Field layout from the vendor CAL (clkout_exynos9810.c, ra.c):
SEL = bits 13:8 (0 = TCXO), bit 0 = disable (1 = gated).
Device memory needs an aligned 32-bit access, so it goes through ctypes on
the mapped page (as s9p-acpm does); the mapping is only read.
"""
import ctypes
import mmap
import os
import sys

PAGE = 0x14060000
OFF = 0xA0C

fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=PAGE)
base = ctypes.addressof(ctypes.c_uint32.from_buffer(m))
v = ctypes.c_uint32.from_address(base + OFF).value
print(f"CLKOUT1 @{PAGE + OFF:#010x} = {v:#010x}: SEL={(v >> 8) & 0x3f:#x} "
      f"DISABLE={v & 1} -> {'GATED' if v & 1 else 'enabled'}")
sys.exit(3 if v & 1 else 0)
