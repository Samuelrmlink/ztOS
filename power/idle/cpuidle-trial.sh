#!/bin/sh
# cpuidle-trial.sh start|stop: the approved per-core cpuidle trial (ROADMAP
# 8.48, owner approval 2026-11-22). Run as root inside `ztlock run awake-drain`.
#
# start: the aodp deadman (sysrq reboot after 8 min without a host
#        heartbeat), kernel.panic_on_oops=1, a zrn-sleepd inhibit that expires
#        by itself in 40 min (timed-*: its mtime is the expiry, so it can't be
#        left behind), the stale 10-18 mark removed, then
#        ZRN_CPUIDLE_CPUS=0x13 zrn-cpuidle try (CPUs 0, 1, 4; per-core C2
#        only: SICD, s2idle_sicd and cluster power-down stay at their default,
#        off). The unit stays disabled.
# stop:  rmmod zrn-cpuidle, the inhibit removed, panic_on_oops back to 0, the
#        deadman stopped. Safe to run more than once.
set -u
INH=/run/zrn-sleepd/inhibit/timed-cpuidle
case "${1:-}" in
start)
	# the module must be for this kernel: release in its vermagic, and (no
	# MODVERSIONS here) the config it was built against: ABI=<hash> from
	# scripts/module-abi.sh of that config, checked against /proc/config.gz
	ko=/usr/lib/modules/$(uname -r)/extra/zrn-cpuidle.ko
	case "$(modinfo -F vermagic $ko)" in "$(uname -r) "*) ;; *) echo "vermagic mismatch: not loading" >&2; exit 1 ;; esac
	if [ -n "${ABI:-}" ]; then
		have=$(zcat /proc/config.gz | grep '^CONFIG_' |
			grep -vE '^CONFIG_(CMDLINE|INITRAMFS_[A-Z0-9_]+|EXTRA_FIRMWARE|EXTRA_FIRMWARE_DIR|PAHOLE_VERSION|RUSTC_VERSION|RUSTC_LLVM_VERSION|RUST_IS_AVAILABLE|BINDGEN_VERSION_TEXT)=' |
			LC_ALL=C sort | sha256sum | cut -d' ' -f1)
		[ "$have" = "$ABI" ] || { echo "kernel config ABI $have is not the module's $ABI: not loading" >&2; exit 1; }
	fi
	mkdir -p /run/zrn-sleepd/inhibit /var/tmp/aodp
	touch /var/tmp/aodp/hb
	systemctl is-active -q aodp-deadman ||
		systemd-run --unit=aodp-deadman --collect bash /home/samuel/aodp/deadman.sh 480
	sysctl -qw kernel.panic_on_oops=1
	: > $INH.tmp && touch -d "@$(( $(date +%s) + 2400 ))" $INH.tmp && mv -f $INH.tmp $INH
	rm -f /var/lib/zrn-cpuidle/loading
	sync
	ZRN_CPUIDLE_CPUS=0x13 /usr/local/sbin/zrn-cpuidle try
	r=$?
	echo "driver: $(cat /sys/devices/system/cpu/cpuidle/current_driver 2>/dev/null)"
	grep -H . /sys/module/zrn_cpuidle/parameters/sicd /sys/module/zrn_cpuidle/parameters/s2idle_sicd \
		/sys/module/zrn_cpuidle/parameters/s2idle_cpd /sys/module/zrn_cpuidle/parameters/cpus 2>/dev/null
	exit $r ;;
stop)
	[ -d /sys/module/zrn_cpuidle ] && rmmod zrn_cpuidle && echo "zrn-cpuidle unloaded"
	rm -f $INH
	sysctl -qw kernel.panic_on_oops=0
	systemctl stop aodp-deadman 2>/dev/null
	echo "driver: $(cat /sys/devices/system/cpu/cpuidle/current_driver 2>/dev/null || echo none)," \
		"inhibit gone: $([ -e $INH ] && echo no || echo yes)," \
		"panic_on_oops=$(sysctl -n kernel.panic_on_oops), unit $(systemctl is-enabled zrn-cpuidle 2>/dev/null)" ;;
*)
	echo "usage: cpuidle-trial.sh start|stop" >&2; exit 2 ;;
esac
