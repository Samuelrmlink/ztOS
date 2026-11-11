#!/usr/bin/env python3
"""The Exynos 9810 DisplayPort TX bring-up driver (kernel/exynos9810-dp.c) in the tree.

Copies it to drivers/gpu/drm/s9p/exynos9810-dp.c (every run, so the tree has
the current version) and adds its Kconfig symbol DRM_EXYNOS9810_DP and
Makefile line once, next to the port's DECON driver. build-boot.sh enables
the symbol with --dp-altmode only; the DT node comes from make-dts.py
--dp-altmode.
"""
from pathlib import Path
import shutil
import sys

tree = Path(sys.argv[1])
here = Path(__file__).resolve().parent
MARK = 'zboot: exynos9810 dp'
d = tree / 'drivers/gpu/drm/s9p'
if not d.is_dir():
    sys.exit('patch-exynos9810-dp: drivers/gpu/drm/s9p missing (the port drivers first)')
shutil.copyfile(here / 'exynos9810-dp.c', d / 'exynos9810-dp.c')


def append(rel, text):
    p = tree / rel
    s = p.read_text()
    if MARK in s:
        return
    p.write_text(s.rstrip('\n') + '\n' + text)


append('drivers/gpu/drm/s9p/Kconfig', '''
# zboot: exynos9810 dp
config DRM_EXYNOS9810_DP
	bool "Exynos 9810 DisplayPort TX over USB-C (bring-up)"
	depends on OF && ARM64 && TYPEC=y && MFD_SYSCON
	help
	  The combo PHY in DP mode and the DP link controller's AUX channel,
	  driven from the USB-C connector's DP Alt Mode state (a typec mode
	  and orientation switch). Staged bring-up through debugfs
	  exynos9810-dp/step; no display output yet. Board wiring from the
	  device tree (docs/dp-altmode.md).
''')
append('drivers/gpu/drm/s9p/Makefile', '''# zboot: exynos9810 dp
obj-$(CONFIG_DRM_EXYNOS9810_DP) += exynos9810-dp.o
''')
print('patch-exynos9810-dp: ok')
