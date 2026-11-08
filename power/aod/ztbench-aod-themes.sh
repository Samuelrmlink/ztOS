#!/bin/bash
# ztbench item aod-themes (~8 min): the current with each AOD theme at the lit-room level,
# against the dark (aod-themes-power.sh). Run by ztbench as root with OUT and ZTB_LOCKED=1.
D=/home/samuel/ztdev-aod2/power/aod
as_user() { runuser -u samuel -- env HOME=/home/samuel XDG_RUNTIME_DIR=/run/user/1000 "$@"; }
[ -d "$D" ] || { echo "no $D" > "$OUT/summary"; exit 2; }
as_user bash $D/aod-themes-power.sh
rc=$?
cp /var/tmp/aod-themes-power.log "$OUT/"
{
	echo "aod-themes (lit-room level; the AOD's cost is each line less the dark's):"
	grep -E "^[a-z]+ +[0-9]+ mA" "$OUT/aod-themes-power.log"
} > "$OUT/summary"
cat "$OUT/summary"
exit $rc
