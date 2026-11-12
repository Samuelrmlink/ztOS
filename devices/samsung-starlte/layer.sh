# The Galaxy S9 (starlte) device layer, run by the installer inside the build
# namespace after the GSI part, with $R (the root), $DEVSRC (this folder:
# vendor/, overlay/, packages.txt), $DEVROOT (the device repository: kernel/,
# firmware/, prebuilts/...; ztOSMobile itself before the split), $P (pacman),
# step and ch (run in the root). Everything here is the S9's own: the GSI
# carries none of it.
D=$DEVSRC

step "adding the Galaxy S9 device layer"
# (packages.txt went in with the GSI's packages, and the kernel was held
# back from pacman before the first update: build.py)

# ztOSIAL: the manifest and the S9's backends
install -d "$R/vendor/ztos"
cp -a "$D/vendor/ztos/." "$R/vendor/ztos/"
chown -R 0:0 "$R/vendor"

cp -a "$D/overlay/." "$R/"                   # the phone's own: charger, panel, sensor hub, modem, sleep
if [ -f "$R/etc/fstab" ]; then
	sed -i 's/\([[:space:]]\)default\([,[:space:]]\)/\1defaults\2/g' "$R/etc/fstab"
fi
# systemd-networkd answers DHCP on the USB link, and does nothing else: NetworkManager
# has Wi-Fi and Ethernet. Arch Linux ARM's en.network and eth.network, and the older
# image's 20-wlan.network, would have both managing the same interface.
find "$R/etc/systemd/network" -maxdepth 1 -name '*.network' ! -name 10-zboot-usb.network -delete
# the panel's brightness table is made from its calibration (ztos-panel-bl)
install -Dm755 "$DEVROOT/kernel/s9p-bl-table.py" "$R/usr/local/lib/ztos/s9p-bl-table.py"
# SYS_SLEEP needs its backend and RTC/TMU helpers as well as the daemon.
PW=$R/usr/local/libexec/ztos-power
install -d "$PW/mif-trial"
install -m755 "$DEVROOT/third_party/s9plus/extras/debug-tools/files/usr/local/sbin/s9p-acpm" "$PW/s9p-acpm"
install -m755 "$DEVROOT/power/rtc/initialize-calendar.py" \
	"$DEVROOT/power/suspend/load-acpm-tmu.py" "$PW/mif-trial/"
install -m644 "$DEVROOT/prebuilts/power/zrn-sleep-init.ko" \
	"$DEVROOT/prebuilts/power/zrn-exynos-rtc.ko" \
	"$DEVROOT/prebuilts/power/exynos9810_acpm_tmu.fw" "$PW/mif-trial/"
install -Dm644 -t "$R/usr/lib/firmware/brcm" "$DEVROOT"/firmware/bcmdhd* "$DEVROOT"/firmware/nvram*
install -Dm644 "$DEVROOT/third_party/s9plus/firmware/bcm4361B2_semco.hcd" \
	"$R/usr/lib/firmware/brcm/BCM.samsung,starlte.hcd"
for f in "$DEVROOT"/firmware/calliope_*.bin; do
	[ -f "$f" ] && install -Dm644 -t "$R/usr/lib/firmware" "$f"
done
# Modules go by kernel release (uname -r): 7.1.0-zboot, and 7.1.0-zboot-wd for a
# kernel with 32-bit compat (--waydroid), whose structures differ (build-boot.sh).
# The loaders find them under /usr/lib/modules/$(uname -r)/extra.
KREL=7.1.0-zboot
install -d "$R/usr/lib/modules/$KREL/extra"
install -d "$R/usr/local/lib"
# which modules: pick-modules.sh, which refuses stale and wrong-kernel ones
MODULES=$(sh "$D/pick-modules.sh" "$DEVROOT") || exit 1
for k in $MODULES "$DEVROOT/prebuilts/power/zrn-sleep-init.ko" "$DEVROOT/prebuilts/power/zrn-exynos-rtc.ko"; do
	install -m644 "$k" "$R/usr/lib/modules/$KREL/extra/"
	case $k in */zrn-modem.ko) install -m644 "$k" "$R/usr/local/lib/zrn-modem.ko" ;; esac
done
if [ -f "$DEVROOT/build/linux-7.1/out/modules.builtin" ] &&
	[ "$(cat "$DEVROOT/build/linux-7.1/out/include/config/kernel.release" 2> /dev/null)" = "$KREL" ]; then
	install -m644 "$DEVROOT/build/linux-7.1/out/modules.builtin" "$R/usr/lib/modules/$KREL/"
	install -m644 "$DEVROOT/build/linux-7.1/out/modules.builtin.modinfo" "$R/usr/lib/modules/$KREL/"
fi
# module sets from scripts/build-bt-modules.sh (out/modules-<release>/), each
# built against one kernel config, into that release's own directory: the
# -wd set for a Waydroid kernel, and a rebuilt 7.1.0-zboot set over the picks
for set in "$DEVROOT"/out/modules-*/; do
	[ -f "$set/RELEASE" ] && [ -f "$set/ABI" ] || continue
	rel=$(cat "$set/RELEASE")
	(cd "$set" && sha256sum --quiet -c SHA256SUMS) || { echo "$set: damaged" >&2; exit 1; }
	install -d "$R/usr/lib/modules/$rel/extra"
	install -m644 "$set"/*.ko "$R/usr/lib/modules/$rel/extra/"
	install -m644 "$set/ABI" "$R/usr/lib/modules/$rel/extra/ztos-modules.abi"
	for f in modules.builtin modules.builtin.modinfo; do
		[ -f "$set/$f" ] && install -m644 "$set/$f" "$R/usr/lib/modules/$rel/"
	done
	echo "modules for $rel: $(ls "$set"/*.ko | wc -l) from $set"
done
for moddir in "$R"/usr/lib/modules/*-zboot*; do
	[ -d "$moddir" ] && ch depmod -a "$(basename "$moddir")" > /dev/null 2>&1 || true
done
for moddir in "$R"/usr/lib/modules/*; do
	[ -d "$moddir" ] || continue
	kver=$(basename "$moddir")
	case $kver in *-zboot*) continue ;; esac
	install -d "$moddir/extra"
	for k in $MODULES; do
		install -m644 "$k" "$moddir/extra/"
	done
	ch depmod -a "$kver" > /dev/null 2>&1 || true
done
chmod 755 "$R"/usr/local/sbin/* 2>/dev/null || true

# (not zrn-cpuidle: it can stop the phone, so it's opt-in, `zrn-cpuidle keep`. The
# prebuilt module also predates the script's parameters; on 2026-10-18 it oopsed on
# a fresh install's first boot, which is how that phone got its held-back mark.)
DEVICE_UNITS="systemd-networkd ztos-variant zboot-boot-ok zboot-usb-gadget zboot-firstboot zboot-charge-hold zrn-therm zrn-thermald zrn-charge ztoscirrusaudio
	ztoscsf-sensorhub ztoscsf-sensord ztoscorenetworking-modem ztoscorenetworking zrnperfd ztprofile zrn-ledd zrn-rtc zrn-sleepd
	ztos-panel-selfmask ztos-panel-selfmask-again.timer ztos-panel-bl zrn-bluetooth bluetooth ztos-gnss-gate ztos-gnss-lto.timer ztos-waydroid-location"
# the 3.5 mm jack: the headphone output and pausing on unplug (a user service)
ch systemctl --global enable ztos-headsetd.service
