#!/bin/bash
# ztbench item aod-power (~10 min): the phone's current with the screen dark, with the AOD
# at its floor and at its ceiling (aod-power2.sh), and the hours each would give. Run by
# ztbench as root with OUT and ZTB_LOCKED=1, on battery.
D=/home/samuel/ztdev-aod2/power/aod
CAPACITY_MAH=${CAPACITY_MAH:-3000}	# the S9's battery, as new
as_user() { runuser -u samuel -- env HOME=/home/samuel XDG_RUNTIME_DIR=/run/user/1000 "$@"; }
[ -d "$D" ] || { echo "no $D" > "$OUT/summary"; exit 2; }
as_user bash $D/aod-power2.sh
rc=$?
cp /var/tmp/aod-power2.log "$OUT/"
{
	echo "aod-power (cable out, Wi-Fi associated in power save, SoC sleep off as on this phone):"
	for what in "dark" "AOD floor" "AOD ceiling"; do
		# the first "N mA" of each line is the battery's
		grep ": $what" "$OUT/aod-power2.log" | grep -oE '^[^I]* -?[0-9]+ mA' | grep -oE -- '-?[0-9]+ mA$' |
			cut -d' ' -f1 | awk -v w="$what" -v cap="$CAPACITY_MAH" '{ s += $1; n++ }
				END { if (n && s > 0) printf "  %-12s %4d mA (mean of %d), %.1f h on %d mAh\n", w, s / n, n, cap / (s / n), cap }'
	done
} > "$OUT/summary"
cat "$OUT/summary"
exit $rc
