#!/bin/bash
# The host's half of deadman.sh: touch the phone's heartbeat every minute
# while it can be reached. Stop it (Ctrl-C, kill) only after the phone's
# deadman has been stopped, or the phone reboots 8 minutes later.
#   ZT_SSH='-i ~/.ssh/ztos_phone samuel@ztos' deadman-host.sh [HOURS]
END=$(( $(date +%s) + ${1:-10} * 3600 ))
while [ "$(date +%s)" -lt "$END" ]; do
	if ssh -o ConnectTimeout=10 -o BatchMode=yes $ZT_SSH 'touch /var/tmp/aodp/hb' 2>/dev/null; then
		echo "$(date '+%T') ok"
	else
		echo "$(date '+%T') unreachable"
	fi
	sleep 60
done
