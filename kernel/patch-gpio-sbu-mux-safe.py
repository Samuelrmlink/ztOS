#!/usr/bin/env python3
"""gpio-sbu-mux: leave the SBU switch as it is in the USB Safe State.

drivers/usb/typec/mux/gpio-sbu-mux.c opens the SBU switch for TYPEC_STATE_SAFE,
which typec_displayport sets around Enter Mode and before every Configure.
Samsung's S9 turns the switch on when Discover SVIDs finds DisplayPort, then
waits 100 ms, and keeps it on through Enter Mode and Configure until unplug
(dpu_9810/displayport_drv.c:2867-2890, :2960-2975, :2984, :2943-2958); a
dock on the S9 raises HPD after that. max77705-usbc does the same: a DP state
at SVID discovery, USB state at detach. So here SAFE changes nothing, USB
still opens the switch, and an unplug (orientation none) still does.

Applied by build-boot.sh --dp-altmode only (its private kernel tree).
"""
from pathlib import Path
import sys

p = Path(sys.argv[1]) / 'drivers/usb/typec/mux/gpio-sbu-mux.c'
s = p.read_text()
MARK = 'zboot: sbu safe'
if MARK in s:
    print('patch-gpio-sbu-mux-safe: already applied')
    sys.exit(0)
old = '''	switch (state->mode) {
	case TYPEC_STATE_SAFE:
	case TYPEC_STATE_USB:
		sbu_mux->enabled = false;
		break;
'''
new = '''	switch (state->mode) {
	case TYPEC_STATE_SAFE:	/* zboot: sbu safe -- as it is, as Samsung's S9 */
		break;
	case TYPEC_STATE_USB:
		sbu_mux->enabled = false;
		break;
'''
if s.count(old) != 1:
    sys.exit('patch-gpio-sbu-mux-safe: gpio_sbu_mux_set() changed')
p.write_text(s.replace(old, new))
print('patch-gpio-sbu-mux-safe: ok')
