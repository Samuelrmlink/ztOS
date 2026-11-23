#!/bin/bash
# cpuidle-watch.sh MINUTES: host-side watch during a zrn-cpuidle trial
# (ROADMAP 8.48). Every 30 s: ping, an ssh round trip that also touches the
# aodp deadman's heartbeat, and the C2 residency per CPU. One line per check;
# "SSH-HUNG ping=ok" is the known failure (a timer sleeper never woken).
MIN=${1:-30}
P='ssh -i ~/.ssh/ztos_phone -o IdentitiesOnly=yes -o UserKnownHostsFile=~/.ssh/known_hosts_ztos -o HostKeyAlias=172.16.42.1 -o ConnectTimeout=10 samuel@ztos'
end=$(( $(date +%s) + MIN * 60 ))
while [ "$(date +%s)" -lt "$end" ]; do
	ping -c1 -W3 ztos >/dev/null 2>&1 && pg=ok || pg=lost
	out=$(timeout 20 $P 'touch /var/tmp/aodp/hb; for c in /sys/devices/system/cpu/cpu[0-9]*/cpuidle/state1; do [ -e $c/usage ] && printf "%s:%s/%sms " $(echo $c | cut -d/ -f6) $(cat $c/usage) $(( $(cat $c/time) / 1000 )); done; printf "drv=%s up=%s" "$(cat /sys/devices/system/cpu/cpuidle/current_driver)" "$(cut -d" " -f1 /proc/uptime)"' 2>&1) && st=ok || st=SSH-HUNG
	echo "$(TZ=America/Chicago date +%T) $st ping=$pg ${out:0:300}"
	sleep 30
done
