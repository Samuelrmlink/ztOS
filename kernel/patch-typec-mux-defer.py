#!/usr/bin/env python3
"""Type-C switch/mux lookup: defer when the switch is not registered yet.

drivers/usb/typec/mux.c, typec_switch_match() and typec_mux_match(): when the
graph names a switch (orientation-switch / mode-switch) whose device is not
registered yet, class_find_device() returns NULL and the function means to
return ERR_PTR(-EPROBE_DEFER). Before that it runs its "skip duplicates" loop,
comparing to_typec_switch_dev(NULL), which is NULL (dev is the first member),
with the caller's sw_devs[] / mux_devs[], a stack array nobody initialised.
Any zero there makes the missing switch a "duplicate" and the lookup returns
NULL, "no switch connected": the port is registered without its switch and
mux for good. Seen on the S9 (dp2): the port came up 25 ms before the SBU
mux, and the mux was never driven.

The fix: no device, defer, before the loop; and the arrays zeroed.
"""
from pathlib import Path
import sys

tree = Path(sys.argv[1])
p = tree / 'drivers/usb/typec/mux.c'
s = p.read_text()
MARK = 'zboot: typec mux defer'
if MARK in s:
    print('patch-typec-mux-defer: already applied')
    sys.exit(0)


def sub(old, new, count=1):
    global s
    if s.count(old) != count:
        sys.exit(f'patch-typec-mux-defer: expected {count}x {old[:60]!r}, found {s.count(old)}')
    s = s.replace(old, new)


sub('''	dev = class_find_device(&typec_mux_class, NULL, fwnode,
				switch_fwnode_match);

	/* Skip duplicates */''', '''	dev = class_find_device(&typec_mux_class, NULL, fwnode,
				switch_fwnode_match);
	if (!dev)	/* zboot: typec mux defer */
		return ERR_PTR(-EPROBE_DEFER);

	/* Skip duplicates */''')
sub('''	dev = class_find_device(&typec_mux_class, NULL, fwnode,
				mux_fwnode_match);

	/* Skip duplicates */''', '''	dev = class_find_device(&typec_mux_class, NULL, fwnode,
				mux_fwnode_match);
	if (!dev)	/* zboot: typec mux defer */
		return ERR_PTR(-EPROBE_DEFER);

	/* Skip duplicates */''')
sub('	struct typec_switch_dev *sw_devs[TYPEC_MUX_MAX_DEVS];\n	struct typec_switch *sw;\n',
    '	struct typec_switch_dev *sw_devs[TYPEC_MUX_MAX_DEVS] = {};\n	struct typec_switch *sw;\n')
sub('	struct typec_mux_dev *mux_devs[TYPEC_MUX_MAX_DEVS];\n	struct typec_mux *mux;\n',
    '	struct typec_mux_dev *mux_devs[TYPEC_MUX_MAX_DEVS] = {};\n	struct typec_mux *mux;\n')
p.write_text(s)
print('patch-typec-mux-defer: ok')
