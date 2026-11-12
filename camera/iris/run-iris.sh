#!/bin/sh
# The iris experiment (README.md steps 1-8), on the phone, attended.
# Approved 2026-11-12, run after the fingerprint one.
#	ztlock run iris 'sudo sh ~/iris-probe/run-iris.sh'
# Same refusals as run-fp.sh, plus zrn-flash must be loaded (its i2c-97 bus is
# how the S2MPB02 is reached). The mark is left in place across the load: if
# step 6 faults and the phone resets, the next run refuses. After a reset, the
# console of the dead boot is in /sys/fs/pstore (the "zrn-iris: step ..." lines).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
D=/var/lib/zrn-iris
. "$HERE/run-checks.sh"
checks iris || exit 3
[ -d /sys/module/zrn_flash ] && [ -e /sys/bus/i2c/devices/i2c-97 ] ||
	{ echo "iris: zrn_flash (i2c-97) not loaded; nothing done"; exit 3; }
[ -d /sys/module/zrn_iris_probe ] && { echo "iris: probe already loaded; nothing done"; exit 3; }
LOG=$D/iris-$(date +%Y%m%d-%H%M%S).log
mark "zrn-iris-probe insmod $(date +%T): if you read this after a reset, step 6 (or earlier) hung"
insmod "$HERE/zrn-iris-probe.ko"
rc=$?
dmesg | grep 'zrn-iris' | tail -40 > "$LOG"
cat /sys/kernel/debug/zrn-iris-probe/result >> "$LOG" 2>/dev/null
cat "$LOG"
[ $rc = 0 ] && rmmod zrn_iris_probe
unmark
echo "insmod exit $rc; log $LOG"
exit $rc
