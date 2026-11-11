#!/bin/sh
# exp.sh CASE...: screen-off A/B toggles for the Always-on lump (root, under ztlock).
# Each case: set, measure, undo; a baseline "B" between them. Everything is undone
# on exit, whatever happens.
D=/var/tmp/always-on
M="$D/meas"
SETTLE=${SETTLE:-40} N=${N:-90}
CTL=/run/user/1000/ztmetro.ctl
STOPPED=""
WIFI_WAS=$(nmcli radio wifi)
DWC=/sys/devices/platform/soc@0/10c00000.usb/power/control
DWC_WAS=$(cat $DWC)
# Waydroid: ztmetro freezes it with the screen off already; put it back as found
FRZ=/sys/fs/cgroup/lxc.payload.waydroid/cgroup.freeze
FRZ_WAS=$(cat $FRZ 2>/dev/null)

pids() { for n in "$@"; do pgrep -x "$n"; done | tr '\n' ' '; }
stop() { STOPPED="$STOPPED $*"; kill -STOP $* 2>/dev/null; }
cont() { [ -n "$STOPPED" ] && kill -CONT $STOPPED 2>/dev/null; STOPPED=""; }
undo() {
	cont
	[ -n "$FRZ_WAS" ] && echo "$FRZ_WAS" > $FRZ
	echo "$DWC_WAS" > $DWC
	[ "$WIFI_WAS" = enabled ] && [ "$(nmcli radio wifi)" != enabled ] && nmcli radio wifi on
	rm -f /run/zrn-sleepd/inhibit/always-on
}
trap 'undo; echo "$(date +%T) undone"' EXIT
trap 'exit 1' INT TERM HUP

touch /run/zrn-sleepd/inhibit/always-on
grep -q '^screen off' /run/user/1000/ztmetro.state || { echo "screen off" > $CTL; sleep 5; }
echo "$(date +%T) start: wifi=$WIFI_WAS dwc3=$DWC_WAS waydroid_frozen=$FRZ_WAS cpus=$(cat /sys/devices/system/cpu/online)"

for c in "$@"; do
	case $c in
	B) $M B $SETTLE $N ;;
	sensors) stop $(pids lhd ztoscsf-sensord); $M "sensors-stopped" $SETTLE $N; cont ;;
	# with the screen off it is frozen already: this case measures it thawed
	waydroid) echo 0 > $FRZ; $M "waydroid-thawed" $SETTLE $N; echo "$FRZ_WAS" > $FRZ ;;
	daemons) stop $(pids ztacctd zrnperfd ztos-audiod ztos-bt-state ztnotifyd zrn-pushd ztos-ntfyd zrn-ledd ztoscorenetwork ztos-iald); $M "daemons-stopped" $SETTLE $N; cont ;;
	apps) stop $(pids ztusage ztpower ztdesk ztdeskmode xdg-desktop-por); $M "apps-stopped" $SETTLE $N; cont ;;
	tailscale) stop $(pids tailscaled); $M "tailscaled-stopped" $SETTLE $N; cont ;;
	pipewire) stop $(pids pipewire pipewire-pulse wireplumber); $M "pipewire-stopped" $SETTLE $N; cont ;;
	dwc3) echo auto > $DWC; sleep 2; echo "  dwc3 runtime: $(cat ${DWC%control}runtime_status)"; $M "dwc3-auto" $SETTLE $N; echo "$DWC_WAS" > $DWC ;;
	wifi) nmcli radio wifi off; $M "wifi-off" $SETTLE $N; nmcli radio wifi on ;;
	*) echo "unknown case $c" ;;
	esac
done
