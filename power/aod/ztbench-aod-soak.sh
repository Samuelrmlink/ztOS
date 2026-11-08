#!/bin/bash
# ztbench item aod-soak (~22 min): the AOD for 21 minutes, a frame a minute, the 10-minute
# moves, a wake by power, by a double tap and by home (aod-soak.sh), then the shell's crash
# record. Run by ztbench as root with OUT and ZTB_LOCKED=1.
D=/home/samuel/ztdev-aod2/power/aod
as_user() { runuser -u samuel -- env HOME=/home/samuel XDG_RUNTIME_DIR=/run/user/1000 "$@"; }
[ -d "$D" ] || { echo "no $D" > "$OUT/summary"; exit 2; }
: > /var/tmp/aod-soak.log
as_user bash $D/aod-soak.sh 21
cp /var/tmp/aod-soak.log "$OUT/"
coredumpctl list --no-pager --since "-30min" ztmetro > "$OUT/cores" 2>&1
fails=$(grep -c FAIL "$OUT/aod-soak.log")
moves=$(grep -c "aod: place" "$OUT/aod-soak.log")
f0=$(grep -m1 "minute 1:" "$OUT/aod-soak.log" | grep -o "aod_frames [0-9]*" | cut -d' ' -f2)
f1=$(grep "minute 21:" "$OUT/aod-soak.log" | grep -o "aod_frames [0-9]*" | cut -d' ' -f2)
woken=$(grep -c "woken by" "$OUT/aod-soak.log")
cores=$(grep -c ztmetro "$OUT/cores")
{
	echo "aod-soak: $fails failures, $moves moves logged, frames ${f0:-?} -> ${f1:-?} over 20 minutes, $woken wakes, $cores ztmetro cores"
	grep -E "FAIL|woken by|ERROR" "$OUT/aod-soak.log"
} > "$OUT/summary"
cat "$OUT/summary"
[ "$fails" = 0 ] && [ "$cores" = 0 ] && [ "$moves" -ge 2 ]
