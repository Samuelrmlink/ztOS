#!/bin/bash
# Build the zboot boot image for the Galaxy S9 (starlte): mainline 7.1 kernel
# with the Exynos 9810 drivers of the S9+ port (third_party/s9plus), our
# initramfs built in, wrapped in uniLoader, packed as a Samsung boot image.
#
#   scripts/build-boot.sh <name> [options]
#     --fallback boot|recovery   where a watchdog reset / panic lands (default
#                                boot: TWRP on BOOT, this image on RECOVERY)
#     --no-ufs                   leave the UFS host disabled
#     --power                    MAX77705 charger + fuel gauge, P9320 wireless receiver
#     --acpm                     ACPM mailbox to the S2MPS18 PMIC (rails)
#     --cpufreq                  CPU frequency scaling, vendor PLL tables (implies --acpm)
#     --touch                    S6SY761 touch controller (implies --acpm: its
#                                rails have to be power-cycled to answer)
#     --gpu                      Mali-G72 through Panfrost (implies --acpm)
#     --decon                    DECON scanout instead of the bootloader framebuffer
#     --wlan                     BCM4361 over the PCIe root complex, with this
#                                phone's own firmware from firmware/
#     --pm-debug                 suspend stage tests and device PM diagnostics
#     --lockup                   soft/hard lockup detectors that panic (backtrace in ramoops)
#     --waydroid                 the kernel side of the optional ztos-waydroid package:
#                                binder + binderfs, PSI, bridge/veth, nftables NAT,
#                                policy routing, Android netd's xtables matches
#                                (kernel/waydroid.config, docs/waydroid.md)
#     --cmdline "..."            extra kernel command line (zboot.rescue=1 ...)
#     --config-only              stop after the configuration: out/<name>.config,
#                                to build the modules for it first (below)
#     --modules DIR              the module set that goes with this kernel (default
#                                out/modules-<release>, from scripts/build-bt-modules.sh)
#     --no-module-check          build even without a module set made for this config
#     --wifi-nvram FILE          Wi-Fi calibration to build in, for --wlan (default:
#                                firmware/nvram.txt_r02a_b2, the G960N's; see
#                                docs/g960f-differences.md for the G960F's)
#
# Output: out/<name>.img (+ .config, .dts, .release, .sha256). Runs unprivileged;
# the toolchain, kernel tree and busybox live under build/.
#
# Kernel release and modules. The image builds no modules, but the phone loads
# ~26 (Bluetooth, sound, zrn-*) built against the same config with
# scripts/build-bt-modules.sh. This kernel has no MODVERSIONS: a module built
# against another config loads if the release matches and then misreads kernel
# structures. So:
#   - CONFIG_COMPAT (--waydroid) changes the release: 7.1.0-zboot-wd, whose
#     modules live in /lib/modules/7.1.0-zboot-wd; without it 7.1.0-zboot. The
#     kernel refuses a module of the other release (vermagic).
#   - the image is not built unless out/modules-<release>/ABI is this config's
#     scripts/module-abi.sh hash, i.e. its modules were built against it. For a
#     new config: build-boot.sh <name> <options> --config-only, then
#     scripts/build-bt-modules.sh out/<name>.config, then the same build-boot.sh
#     line without --config-only.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
B=$ROOT/build
SRC=$B/linux-7.1
PORT=$ROOT/third_party/s9plus
TC=$B/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin
REF=$ROOT/stock/RECOVERY.img    # TWRP for starlte: a header and DTBH sboot accepts
export PATH=$TC:$PATH LC_ALL=C ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu-

NAME=${1:?image name}
shift
FALLBACK=boot
DTS_FLAGS=()
EXTRA=
WIFI_NVRAM=$ROOT/firmware/nvram.txt_r02a_b2
UFS=1 POWER= ACPM= CPUFREQ= GPU= DECON= WLAN= PMDEBUG= LOCKUP= WAYDROID=
CONFIG_ONLY= MODULES= MODCHECK=1
while [ $# -gt 0 ]; do
	case "$1" in
	--fallback) FALLBACK=$2; shift ;;
	--no-ufs) UFS= ;;
	--power) DTS_FLAGS+=(--power); POWER=1 ;;
	--touch) DTS_FLAGS+=(--touch); ACPM=1 ;;
	--gpu) DTS_FLAGS+=(--gpu); GPU=1; ACPM=1 ;;
	--decon) DTS_FLAGS+=(--decon); DECON=1 ;;
	--wlan) DTS_FLAGS+=(--wlan); WLAN=1 ;;
	--pm-debug) PMDEBUG=1 ;;
	--lockup) LOCKUP=1 ;;
	--waydroid) WAYDROID=1 ;;
	--config-only) CONFIG_ONLY=1 ;;
	--modules) MODULES=$(realpath -m "$2"); shift ;;
	--no-module-check) MODCHECK= ;;
	--acpm) ACPM=1 ;;
	--cpufreq) CPUFREQ=1; ACPM=1 ;;
	--cmdline) EXTRA=$2; shift ;;
	--wifi-nvram) WIFI_NVRAM=$2; shift ;;
	*) echo "unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
[ -n "$UFS" ] && DTS_FLAGS+=(--ufs)
case "$FALLBACK" in
boot) INFORM3=0x12345670 ;;
recovery) INFORM3=0x12345674 ;;
*) echo "--fallback boot|recovery" >&2; exit 2 ;;
esac
mkdir -p "$ROOT/out"
OUT=$ROOT/out/$NAME

echo "=== device tree ==="
DTS=arch/arm64/boot/dts/exynos/exynos9810-starlte-zt.dts
python3 "$ROOT/kernel/make-dts.py" "$PORT/src/mainline-dts/exynos9810-star2lte.dts" \
	"$SRC/$DTS" "${DTS_FLAGS[@]}" --fallback "$FALLBACK"
grep -q starlte-zt "$SRC/arch/arm64/boot/dts/exynos/Makefile" ||
	sed -i 's|^\(\s*\)exynos9810-starlte.dtb\(.*\)$|&\n\1exynos9810-starlte-zt.dtb\2|' \
		"$SRC/arch/arm64/boot/dts/exynos/Makefile"
grep -q starlte-zt "$SRC/arch/arm64/boot/dts/exynos/Makefile"

echo "=== port drivers ==="
# kernel/patch-max77705-charger.py edits the installed charger driver; put the
# port's copy back first, or apply_drivers.sh takes it for upstream drift
CHG=$SRC/drivers/power/supply/max77705_charger.c
grep -q 'zboot: max77705' "$CHG" && install -m644 "$PORT/src/soc/max77705_charger.c" "$CHG"
BUILD=$B bash "$PORT/scripts/build/apply_drivers.sh" "$SRC" | tail -n 3
# our own changes to the port's drivers, applied to the copies in the tree
# the panel brightness (AOR) as /sys/class/backlight/s9p-panel, and the panel
# commands sent with the frame trigger held off; kept here, not in the port
patch -d "$SRC" -p1 --forward --no-backup-if-mismatch < "$ROOT/kernel/s9p-decon-backlight.patch"
grep -q s9p_bl_ops "$SRC/drivers/gpu/drm/s9p/s9p-decon.c" ||
	{ echo "s9p-decon.c has no backlight device after patching" >&2; exit 1; }
# PCIe (the BCM4361 Wi-Fi) survives system sleep: stop the link on suspend and
# replay PHY/host setup on resume, since SYS_SLEEP loses ELBI and iATU state
grep -q exynos9810_pcie_pm_ops "$SRC/drivers/pci/controller/dwc/pcie-exynos9810.c" ||
	patch -d "$SRC" -p1 --forward --no-backup-if-mismatch < "$ROOT/kernel/s9p-pcie-pm.patch"
grep -q exynos9810_pcie_pm_ops "$SRC/drivers/pci/controller/dwc/pcie-exynos9810.c" ||
	{ echo "pcie-exynos9810.c has no suspend/resume PM ops after patching" >&2; exit 1; }
# a link that fails to train after sleep is tried again, and config accesses
# while it is down are answered as an absent device instead of hanging the SoC
grep -q exynos9810_pcie_guard_cfg "$SRC/drivers/pci/controller/dwc/pcie-exynos9810.c" ||
	patch -d "$SRC" -p1 --forward --no-backup-if-mismatch < "$ROOT/kernel/s9p-pcie-resume-retry.patch"
grep -q exynos9810_pcie_guard_cfg "$SRC/drivers/pci/controller/dwc/pcie-exynos9810.c" ||
	{ echo "pcie-exynos9810.c has no resume retry after patching" >&2; exit 1; }
python3 "$ROOT/kernel/patch-decon-wake.py" "$SRC"
python3 "$ROOT/kernel/patch-decon-flip.py" "$SRC"
python3 "$ROOT/kernel/patch-decon-panel-lock.py" "$SRC"
python3 "$ROOT/kernel/patch-decon-panel-io.py" "$SRC"
python3 "$ROOT/kernel/patch-suspend-mpidr.py" "$SRC"
python3 "$ROOT/kernel/patch-ufs-suspend.py" "$SRC"
python3 "$ROOT/kernel/patch-touch-suspend.py" "$SRC"
# a hard press at the bottom centre is a home key on its own device (ROADMAP 7.3)
python3 "$ROOT/kernel/patch-touch-force.py" "$SRC"
python3 "$ROOT/kernel/patch-pinctrl-resume.py" "$SRC"
# the firmware's roams are reported to the host the way Samsung's driver does
python3 "$ROOT/kernel/patch-brcmfmac-roam.py" "$SRC"
# MAX77705 charger: input_current_limit is a ceiling the limit ramps up to,
# stock-like AICL back-off (300 mA floor) instead of mainline's ratchet that
# wraps to 3.2 A, the battery's charge current, and max77705-wireless (WCIN)
[ -n "$POWER" ] && python3 "$ROOT/kernel/patch-max77705-charger.py" "$SRC"
# USB OTG: the charger's CHGIN boost as the USB-C VBUS regulator (DT
# usb-otg-vbus, --power only), and the USB 2.0 PHY's host mode for dwc3
[ -n "$POWER" ] && python3 "$ROOT/kernel/patch-max77705-otg.py" "$SRC"
python3 "$ROOT/kernel/patch-usbphy-host.py" "$SRC"
# the wireless charging receiver (IDT P9320), built in with --power
python3 "$ROOT/kernel/patch-p9320.py" "$SRC"
# the MAX77705 USB-C controller driver (replaces s9p-max77705-muic with --power)
python3 "$ROOT/kernel/patch-max77705-usbc.py" "$SRC"

echo "=== initramfs ==="
LIST=$B/initramfs.list
cat > "$LIST" <<EOF
dir /bin 0755 0 0
dir /dev 0755 0 0
dir /proc 0755 0 0
dir /sys 0755 0 0
dir /run 0755 0 0
dir /tmp 1777 0 0
dir /etc 0755 0 0
dir /newroot 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/null 0666 0 0 c 1 3
nod /dev/kmsg 0600 0 0 c 1 11
file /bin/busybox $B/busybox-arm64 0755 0 0
slink /bin/sh busybox 0777 0 0
file /init $ROOT/initramfs/init 0755 0 0
EOF

echo "=== config ==="
cd "$SRC"
[ -f out/.config ] || make -s O=out defconfig
cfg() { ./scripts/config --file out/.config "$@"; }
for sym in ARCH_ACTIONS ARCH_AIROHA ARCH_SUNXI ARCH_ALPINE ARCH_APPLE ARCH_BCM \
	ARCH_BCM2835 ARCH_BCMBCA ARCH_BRCMSTB ARCH_BERLIN ARCH_BLAIZE ARCH_EXYNOSAUTOV9 \
	ARCH_SPARX5 ARCH_K3 ARCH_LG1K ARCH_HISI ARCH_KEEMBAY ARCH_MEDIATEK ARCH_MESON \
	ARCH_MVEBU ARCH_MXC ARCH_LAYERSCAPE ARCH_MA35 ARCH_NPCM ARCH_NXP ARCH_QCOM \
	ARCH_REALTEK ARCH_RENESAS ARCH_ROCKCHIP ARCH_S32 ARCH_SEATTLE ARCH_INTEL_SOCFPGA \
	ARCH_STM32 ARCH_SYNQUACER ARCH_TEGRA ARCH_TESLA_FSD ARCH_SPRD ARCH_THUNDER \
	ARCH_THUNDER2 ARCH_UNIPHIER ARCH_VEXPRESS ARCH_VISCONTI ARCH_XGENE ARCH_ZYNQMP \
	ARCH_SOPHGO ARCH_STM32MP ARCH_MMP ARCH_WPCM450 ARCH_VT8500 \
	SOUND SND MEDIA_SUPPORT PCI PCIE_DW ACPI EFI NUMA XEN KVM VIRTUALIZATION \
	VIRTIO_MENU INFINIBAND RDMA_RXE MTD ATA MD NVME_CORE SCSI_LOWLEVEL \
	ETHERNET WLAN WWAN USB_NET_DRIVERS PHYLIB NET_VENDOR_SAMSUNG \
	INPUT_JOYSTICK INPUT_TABLET INPUT_MISC \
	HID_SUPPORT IIO HWMON W1 FUSION FIREWIRE MACINTOSH_DRIVERS \
	BT CAN NFC WIRELESS RFKILL \
	BTRFS_FS XFS_FS F2FS_FS GFS2_FS OCFS2_FS JFS_FS NTFS3_FS EROFS_FS SQUASHFS \
	NFS_FS NFSD CIFS CEPH_FS 9P_FS CODA_FS AFS_FS ORANGEFS_FS \
	DRM_AMDGPU DRM_NOUVEAU DRM_I915 DRM_RADEON DRM_MSM DRM_ETNAVIV DRM_ROCKCHIP \
	DRM_MEDIATEK DRM_TEGRA DRM_VC4 DRM_SUN4I DRM_ARM DRM_KOMEDA DRM_HDLCD DRM_MALI_DISPLAY \
	DRM_PL111 DRM_RCAR_DU DRM_IMX DRM_MXSFB DRM_MESON DRM_TIDSS DRM_VIRTIO_GPU \
	DRM_LIMA DRM_V3D DRM_EXYNOS DRM_TVE200 DRM_LOGICVC DRM_SSD130X \
	DRM_PANEL_BRIDGE DRM_DISPLAY_DP_HELPER DRM_DP_AUX_CHARDEV \
	STAGING COMEDI GREYBUS FPGA MAILBOX_TEST TEE OPTEE \
	SERIAL_8250 I2C_MUX SPI_MEM MTD_SPI_NOR \
	COMPAT DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT FTRACE STACK_TRACER \
	SECURITY_SELINUX SECURITY_APPARMOR SECURITY_SMACK SECURITY_TOMOYO \
	NET_SCHED NET_CLS_ACT IP_VS BRIDGE VLAN_8021Q \
	WATCHDOG_NOWAYOUT \
	USB_G_SERIAL RANDOMIZE_BASE ARM64_VA_BITS_52 ARM64_PA_BITS_52 \
	PSTORE_PMSG PSTORE_COMPRESS STRICT_DEVMEM SCSI_UFS_CRYPTO; do
	cfg --disable "$sym"
done
make -s O=out olddefconfig
for sym in BLK_DEV_INITRD DRM DRM_EXYNOS_BOOTFB DRM_FBDEV_EMULATION \
	FRAMEBUFFER_CONSOLE FRAMEBUFFER_CONSOLE_DETECT_PRIMARY SYSFB_SIMPLEFB \
	ARCH_EXYNOS PINCTRL_EXYNOS KEYBOARD_GPIO INPUT_EVDEV EXYNOS_PMU EXYNOS_PM_DOMAINS \
	DMA_CMA SCSI BLK_DEV_SD SCSI_UFSHCD SCSI_UFSHCD_PLATFORM SCSI_UFS_EXYNOS9810 \
	USB_GADGET USB_LIBCOMPOSITE USB_U_SERIAL USB_U_ETHER USB_F_ACM USB_F_NCM USB_F_ECM \
	USB_F_RNDIS USB_F_MASS_STORAGE USB_CONFIGFS USB_CONFIGFS_SERIAL USB_CONFIGFS_ACM \
	USB_CONFIGFS_NCM USB_CONFIGFS_ECM USB_CONFIGFS_RNDIS USB_CONFIGFS_MASS_STORAGE \
	USB_CONFIGFS_F_FS USB_DWC3 USB_DWC3_GADGET GENERIC_PHY PHY_EXYNOS5_USBDRD \
	I2C I2C_CHARDEV I2C_EXYNOS5 INPUT_TOUCHSCREEN TOUCHSCREEN_S6SY761 \
	MFD_MAX77705 MFD_SIMPLE_MFD_I2C CHARGER_MAX77705 BATTERY_MAX1720X BATTERY_MAX17042 \
	POWER_SUPPLY REGMAP_I2C REGMAP_IRQ EXT4_FS TMPFS DEVTMPFS DEVTMPFS_MOUNT \
	PSTORE PSTORE_RAM PSTORE_CONSOLE MAGIC_SYSRQ DEVMEM \
	POWER_RESET_SYSCON POWER_RESET_SYSCON_POWEROFF SYSCON_REBOOT_MODE \
	WATCHDOG WATCHDOG_SYSFS S3C2410_WATCHDOG ARM64_VA_BITS_48 ARM64_PA_BITS_48 \
	UCLAMP_TASK CMDLINE_FORCE \
	NETFILTER CGROUPS NAMESPACES FHANDLE AUTOFS_FS SECCOMP INOTIFY_USER SIGNALFD \
	SECURITY_LANDLOCK \
	TIMERFD EPOLL UNIX INET IPV6 PACKET TUN ZRAM FUSE_FS; do
	cfg --enable "$sym"
done
# Suspend stage tests are opt-in and do not enter sleep automatically.
if [ -n "$PMDEBUG" ]; then
	cfg --enable PM_DEBUG
	cfg --enable PM_ADVANCED_DEBUG
fi
# A CPU stuck with interrupts on or off panics with its backtrace (into
# ramoops) instead of hanging silently until the hardware watchdog.
if [ -n "$LOCKUP" ]; then
	cfg --enable SOFTLOCKUP_DETECTOR
	cfg --enable BOOTPARAM_SOFTLOCKUP_PANIC
	cfg --enable HARDLOCKUP_DETECTOR
	cfg --enable HARDLOCKUP_DETECTOR_PREFER_BUDDY
	cfg --enable BOOTPARAM_HARDLOCKUP_PANIC
fi
# Everything below this line is a block that hangs the bus when it is read
# while gated or unpowered, so each one is a switch, off by default.
if [ -n "$ACPM" ]; then
	cfg --enable S9P_ACPM
else
	cfg --disable S9P_ACPM
fi
if [ -n "$CPUFREQ" ]; then
	cfg --enable CPU_FREQ
	cfg --enable CPU_FREQ_STAT
	cfg --enable CPU_FREQ_GOV_SCHEDUTIL
	cfg --enable CPU_FREQ_DEFAULT_GOV_SCHEDUTIL
	cfg --enable ARM_S9P_CPUFREQ
	# only the vendor-table driver: the generic ones defconfig pulls in (and
	# their extra governors) are not in the image the phones run
	for sym in CPUFREQ_DT CPUFREQ_DT_PLATDEV ARM_SCMI_CPUFREQ ARM_SCPI_CPUFREQ \
		CPU_FREQ_GOV_ONDEMAND CPU_FREQ_GOV_USERSPACE CPU_FREQ_GOV_CONSERVATIVE \
		CPU_FREQ_GOV_POWERSAVE; do
		cfg --disable "$sym"
	done
else
	cfg --disable CPU_FREQ
	cfg --disable ARM_S9P_CPUFREQ
fi
if [ -n "$POWER" ]; then
	cfg --enable CHARGER_P9320
	# the fuel gauge's input current (IIN: USB or wireless), system current,
	# VBYP and VSYS, as /sys/class/hwmon (max77705-hwmon, a simple-mfd-i2c cell)
	cfg --enable HWMON
	cfg --enable SENSORS_MAX77705
else
	cfg --disable CHARGER_P9320
fi
# MAX77705 USB-C controller: needs the MFD's interrupt domain, so --power only
if [ -n "$POWER" ]; then
	cfg --enable CHARGER_MAX77705_USBC
	# its Type-C port, partner and PD objects (/sys/class/typec, usb_power_delivery)
	cfg --enable TYPEC
else
	cfg --disable CHARGER_MAX77705_USBC
	cfg --disable TYPEC
fi
if [ -n "$GPU" ]; then
	cfg --enable DRM_PANFROST
	cfg --enable IOMMU_SUPPORT
	cfg --enable IOMMU_IO_PGTABLE_LPAE
	cfg --enable ARM_MALI_LPAE
	cfg --enable DRM_GEM_SHMEM_HELPER
else
	cfg --disable DRM_PANFROST
fi
if [ -n "$DECON" ]; then
	cfg --enable DRM_S9P_DECON
	cfg --enable DRM_KMS_HELPER
	cfg --enable DRM_GEM_DMA_HELPER
	# the panel's own brightness (AOR), as a backlight device
	cfg --enable BACKLIGHT_CLASS_DEVICE
else
	cfg --disable DRM_S9P_DECON
fi
if [ -n "$WLAN" ]; then
	# The firmware is this phone's own, out of its VENDOR partition, renamed
	# the way brcmfmac asks for it; the nvram names itself "STAR1 KR", so it
	# is the S9's calibration and not the S9+'s. The firmware and CLM are the
	# same on a G960F; its calibration is not (--wifi-nvram).
	for f in bcmdhd_sta.bin_b2 bcmdhd_clm.blob; do
		[ -s "$ROOT/firmware/$f" ] || { echo "missing firmware/$f" >&2; exit 1; }
	done
	[ -s "$WIFI_NVRAM" ] || { echo "missing $WIFI_NVRAM" >&2; exit 1; }
	cfg --enable PCI
	cfg --enable PCIE_DW
	cfg --enable PCIE_DW_HOST
	cfg --enable PCIE_EXYNOS9810
	cfg --enable WLAN
	cfg --enable WIRELESS
	cfg --enable CFG80211
	cfg --enable BRCMUTIL
	cfg --enable BRCMFMAC
	cfg --enable BRCMFMAC_PCIE
	FW=$B/firmware
	mkdir -p "$FW/brcm"
	cp "$ROOT/firmware/bcmdhd_sta.bin_b2" "$FW/brcm/brcmfmac4361-pcie.bin"
	cp "$WIFI_NVRAM" "$FW/brcm/brcmfmac4361-pcie.txt"
	cp "$ROOT/firmware/bcmdhd_clm.blob" "$FW/brcm/brcmfmac4361-pcie.clm_blob"
	sed -i 's/\r$//' "$FW/brcm/brcmfmac4361-pcie.txt"
	cp "$PORT/firmware/regulatory.db" "$PORT/firmware/regulatory.db.p7s" "$FW/"
	cfg --set-str EXTRA_FIRMWARE_DIR "$FW"
	cfg --set-str EXTRA_FIRMWARE "brcm/brcmfmac4361-pcie.bin brcm/brcmfmac4361-pcie.txt brcm/brcmfmac4361-pcie.clm_blob regulatory.db regulatory.db.p7s"
else
	cfg --set-str EXTRA_FIRMWARE ""
fi
# Waydroid (Android in a container) is an optional package, not the base
# system: its kernel side is a switch. All built in, as this image loads no
# modules. BRIDGE is in the disable list above and comes back only here.
if [ -n "$WAYDROID" ]; then
	while read -r sym val; do
		case "$sym" in ''|'#'*) continue ;; esac
		case "$val" in
		y) cfg --enable "$sym" ;;
		n) cfg --disable "$sym" ;;
		'""') cfg --set-str "$sym" "" ;;
		*) cfg --set-val "$sym" "$val" ;;
		esac
	done < "$ROOT/kernel/waydroid.config"
fi
cfg --set-str INITRAMFS_SOURCE "$LIST"
cfg --set-val CMA_SIZE_MBYTES 128
# The watchdog is armed by uniLoader and again by the clock-gates node, then
# taken over by s3c2410_wdt (tmr_atboot). The kernel pets it until userspace
# opens /dev/watchdog, so a wedged kernel resets into the fallback target.
# Firmware roaming stays on: kernel/patch-brcmfmac-roam.py reports its roams (ROADMAP 1.7)
CMDLINE="console=tty0 console=ramoops-1 loglevel=7 panic=5 keep_bootcon maxcpus=8 s9p_vendor_m3_defaults s3c2410_wdt.tmr_atboot=1 s3c2410_wdt.tmr_margin=60${EXTRA:+ $EXTRA}"
cfg --set-str CMDLINE "$CMDLINE"
make -s O=out olddefconfig
grep -E '^CONFIG_(CMDLINE|SCSI_UFS_EXYNOS9810|USB_CONFIGFS_NCM|S3C2410_WATCHDOG|PSTORE_RAM|DRM_EXYNOS_BOOTFB|PHY_EXYNOS5_USBDRD|USB_DWC3_GADGET|USB_DWC3_MODE)' out/.config ||
	true
grep -E '^(# )?CONFIG_(S9P_ACPM|ARM_S9P_CPUFREQ|DRM_PANFROST|DRM_S9P_DECON|CPU_FREQ)[ =]' out/.config || true
# each =y of the fragment has to survive olddefconfig (one whose dependency
# is off is dropped without a word)
if [ -n "$WAYDROID" ]; then
	missing=
	while read -r sym val; do
		case "$sym" in ''|'#'*) continue ;; esac
		[ "$val" = y ] || continue
		grep -q "^CONFIG_$sym=y$" out/.config || missing="$missing $sym"
	done < "$ROOT/kernel/waydroid.config"
	[ -z "$missing" ] || { echo "--waydroid: not enabled:$missing" >&2; exit 1; }
	echo "waydroid: kernel/waydroid.config applied"
fi
# The release names the module ABI (see the top): 32-bit compat changes
# structures modules use, so a COMPAT kernel gets its own /lib/modules.
LV=-zboot
grep -q '^CONFIG_COMPAT=y$' out/.config && LV=-zboot-wd
cfg --set-str LOCALVERSION "$LV"
make -s O=out olddefconfig
RELEASE=$(sh "$ROOT/scripts/module-abi.sh" --release out/.config "$(make -s kernelversion)")
ABI=$(sh "$ROOT/scripts/module-abi.sh" out/.config)
echo "release $RELEASE, module ABI $ABI"
if [ -n "$CONFIG_ONLY" ]; then
	cp out/.config "$OUT.config"
	echo "CONFIG_OK $OUT.config release=$RELEASE"
	echo "next: scripts/build-bt-modules.sh out/$NAME.config, then this line without --config-only"
	exit 0
fi

echo "=== modules ==="
MODULES=${MODULES:-$ROOT/out/modules-$RELEASE}
if [ -n "$MODCHECK" ]; then
	if [ ! -f "$MODULES/ABI" ]; then
		echo "no module set for $RELEASE in $MODULES. Build it first:" >&2
		echo "  scripts/build-boot.sh $NAME <these options> --config-only" >&2
		echo "  scripts/build-bt-modules.sh out/$NAME.config" >&2
		echo "(or --no-module-check, for an image whose modules you will not ship)" >&2
		exit 1
	fi
	if [ "$(cat "$MODULES/RELEASE")" != "$RELEASE" ] || [ "$(cat "$MODULES/ABI")" != "$ABI" ]; then
		echo "$MODULES was built against another config ($(cat "$MODULES/RELEASE"), ABI $(cat "$MODULES/ABI")):" >&2
		diff <(grep '^CONFIG_' "$MODULES/config" | sort) <(grep '^CONFIG_' out/.config | sort) | head -30 >&2 || true
		echo "rebuild it: build-boot.sh ... --config-only, then scripts/build-bt-modules.sh out/$NAME.config" >&2
		exit 1
	fi
	(cd "$MODULES" && sha256sum --quiet -c SHA256SUMS) ||
		{ echo "$MODULES: modules changed since they were built" >&2; exit 1; }
	echo "modules: $MODULES ($(ls "$MODULES"/*.ko | wc -l)) built for this config"
else
	echo "modules: not checked (--no-module-check)"
fi

echo "=== kernel ==="
make -j"$(nproc)" O=out Image dtbs > "$B/kernel-build.log" 2>&1 || {
	grep -nE '\*\*\*|error:|Error [0-9]|undefined reference' "$B/kernel-build.log" | head -40
	exit 1
}
IMAGE=$SRC/out/arch/arm64/boot/Image
DTB=$SRC/out/arch/arm64/boot/dts/exynos/exynos9810-starlte-zt.dtb
ls -l "$IMAGE" "$DTB"
# what uname -r will say (LOCALVERSION_AUTO could add to it): the modules' release
[ "$(cat out/include/config/kernel.release)" = "$RELEASE" ] || {
	echo "the kernel's release is $(cat out/include/config/kernel.release), not $RELEASE" >&2
	exit 1
}
if grep -q '=m$' out/.config; then
	echo "note: $(grep -c '=m$' out/.config) symbols are =m and are not built"
fi

echo "=== uniLoader ==="
STAGE=$B/uniloader-stage
rm -rf "$STAGE"
cp -r "$PORT/bootloader/uniLoader" "$STAGE"
python3 "$ROOT/kernel/uniloader-failsafe.py" "$STAGE/board/samsung/board-starlte.c" "$INFORM3"
mkdir -p "$STAGE/blob"
cp "$IMAGE" "$STAGE/blob/Image"
cp "$DTB" "$STAGE/blob/dtb"
# The port's validated ramdisk blob: a 20-byte empty gzip (see its
# build_uniloader.sh for why this is not a cpio archive).
printf '' | gzip -n > "$STAGE/blob/ramdisk"
(
	cd "$STAGE"
	make ARCH=aarch64 CROSS_COMPILE=$CROSS_COMPILE star2lte_defconfig
	make ARCH=aarch64 CROSS_COMPILE=$CROSS_COMPILE -j"$(nproc)"
) > "$B/uniloader-build.log" 2>&1 || { tail -n 30 "$B/uniloader-build.log"; exit 1; }
ls -l "$STAGE/uniLoader"

echo "=== boot image ==="
PARTS=$B/parts
mkdir -p "$PARTS"
python3 -I - "$PORT/scripts/build" "$REF" "$PARTS" <<'PY'
import gzip, sys
sys.path.insert(0, sys.argv[1])
from boot_parts import parts
p = parts(open(sys.argv[2], "rb").read())
open(sys.argv[3] + "/dt.img", "wb").write(p["dt"])
open(sys.argv[3] + "/ramdisk.gz", "wb").write(gzip.compress(b"", compresslevel=9, mtime=0))
PY
rm -f "$OUT.img"
python3 -I "$PORT/scripts/build/mkboot.py" --ref "$REF" --kernel "$STAGE/uniLoader" \
	--ramdisk "$PARTS/ramdisk.gz" --dt "$PARTS/dt.img" --cmdline "console=tty0" -o "$OUT.img"
cp "$SRC/out/.config" "$OUT.config"
echo "$RELEASE" > "$OUT.release"
cp "$SRC/$DTS" "$OUT.dts"
echo "$CMDLINE" > "$OUT.cmdline"
(cd "$ROOT/out" && sha256sum "$NAME.img" > "$NAME.img.sha256" && cat "$NAME.img.sha256")
echo "BUILD_OK $OUT.img fallback=$FALLBACK release=$RELEASE"
[ -z "$MODCHECK" ] || echo "modules for it: $MODULES (install-modules.sh, on the phone, before flashing)"
