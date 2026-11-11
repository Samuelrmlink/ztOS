#!/bin/sh
# rec.sh OUT GAP PULSES... : record the raw mic while vt plays pulses
out=$1; gap=$2; shift 2
n=$#
dur=$(( (400 + gap * n + 400) ))
cd "${VIBE_DIR:-$HOME/agent-vibe}"
timeout $(awk "BEGIN{print ($dur+600)/1000}") pw-record --target s9-microphone --rate 48000 --channels 2 --format s16 $out.wav &
sleep 0.5
./vt $gap "$@" > $out.log
wait
cat /sys/bus/platform/devices/max77705-haptic/stats | head -$((n+2)) > $out.stats
ls -l $out.wav
