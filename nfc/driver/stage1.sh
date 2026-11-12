#!/bin/sh
# Stage 0 + stage 1 of nfc/README.md section 7 (owner-approved 2026-11-12).
# Run as root inside `ztlock run nfc`. Stops at the first unexpected state.
#   stage1.sh <dir with zrn-nfc.ko>
D=${1:-$(dirname "$0")}
ACPM=/usr/local/libexec/ztos-power/s9p-acpm
say() { echo "== $*"; }

say "date $(date -Is) battery $(cat /sys/class/power_supply/max170xx_battery/capacity)%"
cap=$(cat /sys/class/power_supply/max170xx_battery/capacity)
[ "$cap" -ge 15 ] || { say "STOP: battery below 15%"; exit 1; }

# the HRM agent's bus must not be loaded; gpp5-0/1 must be free
if lsmod | grep -q '^zrn_hrm'; then say "STOP: zrn_hrm loaded"; exit 1; fi
if lsmod | grep -q '^zrn_nfc'; then say "STOP: zrn_nfc already loaded"; exit 1; fi
pm=$(grep -E 'gpp5-[01]\)' /sys/kernel/debug/pinctrl/10830000.pinctrl*/pinmux-pins)
echo "$pm"
echo "$pm" | grep -q -v UNCLAIMED && { say "STOP: gpp5-0/1 claimed"; exit 1; }
[ -e /dev/i2c-103 ] && { say "STOP: i2c-103 exists"; exit 1; }

say "stage 0: LDO38 (S2MPS18 0x6f) via ACPM"
out=$(python3 "$ACPM" read 1 0x6f); echo "$out"
val=$(echo "$out" | sed -n 's/.*val=\(0x[0-9a-fA-F]*\).*/\1/p')
[ -n "$val" ] || { say "STOP: no ACPM answer"; exit 1; }
mode=$(( (val >> 6) & 3 ))
say "LDO38 = $val (mode $mode)"
[ "$mode" -ne 0 ] || { say "STOP: LDO38 off"; exit 1; }

exp_regs() {
	for r in 0x00 0x01 0x02 0x04 0x05 0x06 0x0c 0x0d 0x0e; do
		printf '%s=%s ' "$r" "$(i2cget -f -y 96 0x22 $r)"
	done; echo
}
say "stage 0: PCAL6524 regs (in 00-02, out 04-06, cfg 0c-0e; VEN=P1.3 FIRM=P1.1)"
exp_regs

say "stage 1: insmod zrn-nfc"
n0=$(dmesg | wc -l)
insmod "$D/zrn-nfc.ko"; rc=$?
dmesg | tail -n +$((n0 + 1))
say "insmod rc=$rc"
grep -E 'gpp5-[01]\)|gpa1-2\)|gpa2-6\)' /sys/kernel/debug/pinctrl/*/pinmux-pins
say "expander after run"
exp_regs
if lsmod | grep -q '^zrn_nfc'; then
	rmmod zrn_nfc; say "rmmod rc=$?"
fi
dmesg | tail -n +$((n0 + 1)) | grep -v 'zrn-nfc: [RT]X\|zrn-nfc: \['
grep -E 'gpp5-[01]\)' /sys/kernel/debug/pinctrl/10830000.pinctrl*/pinmux-pins
[ -e /dev/i2c-103 ] && say "WARN: i2c-103 still present" || say "bus released"
