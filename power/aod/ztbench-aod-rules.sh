#!/bin/bash
# ztbench item aod-rules (~10 min): aod-test.sh and aod-test2.sh, the AOD's rules through
# the shell's ctl. Run by ztbench as root with OUT and ZTB_LOCKED=1 (the lock is held).
# The tests run as the phone's user, from ~/ztdev-aod2/power/aod (feature/aod-2).
D=/home/samuel/ztdev-aod2/power/aod
as_user() { runuser -u samuel -- env HOME=/home/samuel XDG_RUNTIME_DIR=/run/user/1000 "$@"; }
[ -d "$D" ] || { echo "no $D" > "$OUT/summary"; exit 2; }
pgrep -x ztmetro > /dev/null || { echo "no shell running" > "$OUT/summary"; exit 2; }
as_user bash $D/aod-test.sh
as_user bash $D/aod-test2.sh
cp /var/tmp/aod-test.log /var/tmp/aod-test2.log "$OUT/"
pass=$(cat "$OUT"/aod-test*.log | grep -c ' PASS ')
fail=$(cat "$OUT"/aod-test*.log | grep -c ' FAIL ')
{
	echo "aod-rules: $pass passed, $fail failed"
	grep -h ' FAIL ' "$OUT"/aod-test*.log
	grep -h -E "lux [0-9]+:|lux real|auto brightness off" "$OUT/aod-test2.log"
} > "$OUT/summary"
cat "$OUT/summary"
[ "$fail" = 0 ] && [ "$pass" -gt 0 ]
