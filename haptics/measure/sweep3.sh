#!/bin/sh
# sweep3.sh TAG "tau_us od_pct brake_pct brake_max_ms idle_ms" ... : one recording per setting (v3 driver)
P=/sys/module/${MOD:-zrn_haptic_dev}/parameters
tag=$1; shift  # run from VIBE_DIR (default ~/agent-vibe), holding vt and rec.sh
save="$(cat $P/tau_us) $(cat $P/od_pct) $(cat $P/brake_pct) $(cat $P/brake_max_ms) $(cat $P/idle_ms)"
set_p() { set -- $1; echo $1 | sudo tee $P/tau_us >/dev/null; echo $2 | sudo tee $P/od_pct >/dev/null
	echo $3 | sudo tee $P/brake_pct >/dev/null; echo $4 | sudo tee $P/brake_max_ms >/dev/null; echo $5 | sudo tee $P/idle_ms >/dev/null; }
i=0
for cfg in "$@"; do
	set_p "$cfg"
	sh "${VIBE_DIR:-$HOME/agent-vibe}"/rec.sh ${tag}_$i 500 ${PULSES:-7:40 20:63 40:63} >/dev/null 2>&1
	echo "$cfg" > "${VIBE_DIR:-$HOME/agent-vibe}"/${tag}_$i.cfg; echo "${tag}_$i: $cfg"
	i=$((i+1))
done
set_p "$save"
