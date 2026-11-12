#!/bin/bash
# Deadman for panel-AOD and sleep trials (ROADMAP 8.45). Run as root, detached
# from any trial:
#   sudo systemd-run --unit=aodp-deadman --collect bash deadman.sh [LIMIT_S]
# The host touches $HB over ssh every minute (deadman-host.sh). If the file is
# older than LIMIT_S (480 = 8 min) by the wall clock, which goes on while the
# SoC sleeps, the phone is rebooted with sysrq (sync, remount ro, boot): no
# systemd shutdown, no userspace that a hung display or driver could hold up.
# Stop it with: sudo systemctl stop aodp-deadman
HB=/var/tmp/aodp/hb
LIMIT=${1:-480}
LOG=/var/tmp/aodp/deadman.log
mkdir -p /var/tmp/aodp
touch "$HB"
echo "$(date '+%F %T') armed, limit ${LIMIT}s" >> "$LOG"
while sleep 15; do
	age=$(( $(date +%s) - $(stat -c %Y "$HB" 2>/dev/null || echo 0) ))
	if [ "$age" -gt "$LIMIT" ]; then
		echo "$(date '+%F %T') heartbeat ${age}s old: rebooting" >> "$LOG"
		sync
		echo 1 > /proc/sys/kernel/sysrq
		echo s > /proc/sysrq-trigger; sleep 2
		echo u > /proc/sysrq-trigger; sleep 1
		echo b > /proc/sysrq-trigger
	fi
done
