#!/bin/sh
# Stage 0 + stage 1 (1b: LDO38 on/restore; 1c: 150 ms, one retry, class-0 bus)
# of nfc/README.md, owner-approved 2026-11-12.
# Run as root inside `ztlock run nfc`. Stops at the first unexpected state.
#   stage1.sh <dir with zrn-nfc-stage1.ko>
D=${1:-$(dirname "$0")}
ACPM=/usr/local/libexec/ztos-power/s9p-acpm
say() { echo "== $*"; }

say "date $(date -Is) battery $(cat /sys/class/power_supply/max170xx_battery/capacity)%"
cap=$(cat /sys/class/power_supply/max170xx_battery/capacity)
[ "$cap" -ge 15 ] || { say "STOP: battery below 15%"; exit 1; }

# the HRM agent's bus must not be loaded; gpp5-0/1 must be free
if lsmod | grep -q '^zrn_hrm'; then say "STOP: zrn_hrm loaded"; exit 1; fi
if lsmod | grep -q '^zrn_nfc_stage1'; then say "STOP: zrn_nfc_stage1 already loaded"; exit 1; fi
pm=$(grep -E 'gpp5-[01]\)' /sys/kernel/debug/pinctrl/10830000.pinctrl*/pinmux-pins)
echo "$pm"
echo "$pm" | grep -q -v UNCLAIMED && { say "STOP: gpp5-0/1 claimed"; exit 1; }
[ -e /dev/i2c-103 ] && { say "STOP: i2c-103 exists"; exit 1; }
lsmod | grep -q '^i2c_algo_bit' || { say "STOP: i2c_algo_bit not loaded"; exit 1; }

say "stage 0: LDO38 (S2MPS18 0x6f) via ACPM"
out=$(python3 "$ACPM" read 1 0x6f); echo "$out"
val=$(echo "$out" | sed -n 's/.*val=\(0x[0-9a-fA-F]*\).*/\1/p')
[ -n "$val" ] || { say "STOP: no ACPM answer"; exit 1; }
mode=$(( (val >> 6) & 3 ))
say "LDO38 = $val (mode $mode)"
ldo_read() { python3 "$ACPM" read 1 0x6f | sed -n 's/.*val=\(0x[0-9a-fA-F]*\).*/\1/p'; }
restore_ldo() {
	say "stage 1b: restore LDO38 to $val"
	python3 "$ACPM" write 1 0x6f "$val"
	say "LDO38 now $(ldo_read)"
}
if [ "$mode" -eq 0 ]; then
	# stage 1b (owner-approved 2026-11-12): only from the approved 0x2c
	[ "$(printf %d "$val")" -eq $((0x2c)) ] || { say "STOP: LDO38 off but not 0x2c"; exit 1; }
	trap restore_ldo EXIT
	say "stage 1b: LDO38 on (update 0x6f 0x80 mask 0xc0)"
	python3 "$ACPM" update 1 0x6f 0x80 0xc0
	now=$(ldo_read)
	say "LDO38 now $now"
	[ "$(printf %d "$now")" -eq $((0xac)) ] || { say "STOP: LDO38 did not read 0xac"; exit 1; }
	sleep 0.005
fi

exp_regs() {
	for r in 0x00 0x01 0x02 0x04 0x05 0x06 0x0c 0x0d 0x0e; do
		printf '%s=%s ' "$r" "$(i2cget -f -y 96 0x22 $r)"
	done; echo
}
say "stage 0: PCAL6524 regs (in 00-02, out 04-06, cfg 0c-0e; VEN=P1.3 FIRM=P1.1)"
exp_regs

say "stage 1: insmod zrn-nfc"
M="zrn-nfc-run $(date +%s)"; echo "$M" > /dev/kmsg
klog() { dmesg | sed -n "/$M/,\$p" | grep -E "zrn-nfc|i2c|gpio"; }
insmod "$D/zrn-nfc-stage1.ko"; rc=$?
klog
say "insmod rc=$rc"
grep -E 'gpp5-[01]\)|gpa1-2\)|gpa2-6\)' /sys/kernel/debug/pinctrl/*/pinmux-pins
say "expander after run"
exp_regs
if lsmod | grep -q '^zrn_nfc_stage1'; then
	rmmod zrn_nfc_stage1; say "rmmod rc=$?"
fi
klog | tail -2
grep -E 'gpp5-[01]\)' /sys/kernel/debug/pinctrl/10830000.pinctrl*/pinmux-pins
[ -e /dev/i2c-103 ] && say "WARN: i2c-103 still present" || say "bus released"
