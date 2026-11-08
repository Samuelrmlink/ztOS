#!/bin/bash
# PC side of screenoff-ab.sh: fetch its schedule from the phone (before it starts,
# so no polling lands in a measurement), then in each phase's probe window ping
# the LAN and Tailscale addresses (spaced, so each finds the radio asleep) and
# time an ssh to each. ssh first: the window must end before the next phase.
#   probe.sh [LOG]	(PHONE_LAN, PHONE_TS override the addresses)
S="ssh -n -i $HOME/.ssh/ztos_phone -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=10 -o UserKnownHostsFile=$HOME/.ssh/known_hosts_ztos -o HostKeyAlias=172.16.42.1"
LAN=${PHONE_LAN:-192.168.1.214} TS=${PHONE_TS:-100.100.183.45}
OUT=${1:-$(dirname $0)/probe.log}
sched=
while [ -z "$sched" ]; do
	sched=$($S samuel@$LAN 'cat wifipm/schedule 2>/dev/null' 2>/dev/null || $S samuel@$TS 'cat wifipm/schedule 2>/dev/null' 2>/dev/null)
	[ -z "$sched" ] && sleep 30
done
echo "schedule: $sched" | tr '\n' ' ' >> $OUT; echo >> $OUT
echo "$sched" | while read -r p s m e end; do
	[ $(date +%s) -gt $((e + 20)) ] && continue
	while [ $(date +%s) -lt $e ]; do sleep 1; done
	sleep 3
	{
	echo "== phase $p $(date +%T)"
	for ip in $LAN $TS; do
		a=$(date +%s.%N); $S samuel@$ip true 2>/dev/null; rc=$?; b=$(date +%s.%N)
		echo "ssh $ip rc=$rc $(echo "$b - $a" | bc) s"
		sleep 4
	done
	for ip in $LAN $TS; do
		r=""
		for i in 1 2 3 4; do
			t=$(ping -c1 -W2 $ip | sed -n 's/.*time=\([0-9.]*\).*/\1/p'); r="$r ${t:-LOST}"; sleep 4
		done
		echo "ping $ip (spaced 4 s):$r"
	done
	} >> $OUT 2>&1
done
echo "== done $(date +%T)" >> $OUT
