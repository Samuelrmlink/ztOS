#!/bin/bash
# The panel trials' safety net (ROADMAP 8.45): a process of its own that
# follows the kernel log and, at the first sign that DECON's frames stopped
# completing, takes the panel out of its low-power mode with Samsung's exit.
#   sudo systemd-run --unit=aodp-watch --collect bash lpm-watch.sh
# Nothing else here: no screenshots, nothing that can block.
# The exit (star_a3_s0_alpm_exit_cmdtbl + s6e3ha8_aod_exit_aod_cmdtbl, see
# power/aod/PANEL-AOD.md) goes through the kernel's aod_lpm when the kernel
# has it, else through panel_cmd; both keys are closed after it.
DEV=/sys/devices/platform/soc@0/16030000.decon
LOG=/var/tmp/aodp/watch.log
BAD='flip did not latch|vblank wait timed out|did not take DCS|did not take B0|previous shadow update stuck'
mkdir -p /var/tmp/aodp
echo "$(date '+%F %T') watching" >> "$LOG"
dmesg -w --since now 2>/dev/null | while read -r line; do
	if [[ $line =~ $BAD ]]; then
		echo "$(date '+%F %T') MATCH: $line" >> "$LOG"
		if [ -e "$DEV/aod_lpm" ]; then
			echo 0 > "$DEV/aod_lpm" 2>> "$LOG"
		else
			echo 'F0 5A 5A; 76 09 0F 00; 77 00 00; 79 00 00; F0 A5 A5' > "$DEV/panel_cmd" 2>> "$LOG"
			echo 'F0 5A 5A; BB 01; F4 EB 23 0B 17 CA; 53 00; F0 A5 A5' > "$DEV/panel_cmd" 2>> "$LOG"
		fi
		echo 'F0 A5 A5; FC A5 A5' > "$DEV/panel_cmd" 2>> "$LOG"
		echo "$(date '+%F %T') exit sent; $(cat $DEV/aod_lpm 2>/dev/null)" >> "$LOG"
		touch /var/tmp/aodp/tripped
	fi
done
