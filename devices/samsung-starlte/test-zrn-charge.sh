#!/bin/bash
# zrn-charge against a fake sysfs: charger types, AICL, temperature with its
# hysteresis, 9 V, wireless, today's kernel (no max77705-usbc, the MUIC over a
# fake i2cget), unplugging, the log, and the daemon itself. Needs no phone.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
Z=$HERE/overlay/usr/local/sbin/zrn-charge
T=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
fail=0
EP=1800000000	# the wall clock, for the memory's age
R=$T/root PS=$R/sys/class/power_supply
C=$PS/max77705-charger U=$PS/max77705-usbc W=$PS/max77705-wireless B=$PS/max170xx_battery

fresh() {	# new kernel (usbc), or "old": the charger and the gauge only
	rm -rf "$R"; mkdir -p "$C" "$B" "$R/proc" "$R/run/zrn-led" "$R/sys/class/drm/card0-DSI-1"
	echo disabled > "$R/sys/class/drm/card0-DSI-1/enabled"
	echo 100.00 100.00 > "$R/proc/uptime"
	w "$C" online=0 input_current_limit=500000 constant_charge_current=450000 \
		constant_charge_voltage=4350000 charge_behaviour='[auto] inhibit-charge'
	w "$B" temp=250 capacity=50 status=Discharging voltage_now=3900000 constant_charge_voltage_max=4350000
	if [ "${1:-}" != old ]; then
		mkdir -p "$U" "$W"
		w "$U" online=0 usb_type='[Unknown] SDP DCP CDP C PD' charger_type= current_max=0 voltage_now=5000000
		w "$W" online=0 input_current_limit=500000
	fi
}
w() {	# dir name=value...
	local d=$1 kv; shift
	for kv; do printf '%s\n' "${kv#*=}" > "$d/${kv%%=*}"; done
}
tick() { local s; read -r s _ < "$R/proc/uptime"; echo "$(( ${s%.*} + $1 )).00 0" > "$R/proc/uptime"; }
run() {	# one pass; its output in $T/out
	ZRN_CHARGE_ROOT=$R ZRN_CHARGE_DETECT_S=0 ZRN_CHARGE_I2CGET=$T/i2cget ZRN_CHARGE_TRACE=$T/trace ZRN_CHARGE_EPOCH=$EP \
		bash "$Z" > "$T/out" 2>&1
}
lim() {	# what the charger was set to: input charge behaviour (mA)
	local i c b
	read -r i < "$C/input_current_limit"; read -r c < "$C/constant_charge_current"
	read -r b < "$C/charge_behaviour"; b=${b#*[}; b=${b%%]*}
	echo "$((i / 1000)) $((c / 1000)) $b"
}
check() {	# name, wanted, got
	if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got '$3', wanted '$2'"; fail=1; fi
}
plug() {	# usbc charger_type [current_max uA] [voltage uV]
	w "$C" online=1; w "$U" online=1 charger_type="$1" current_max="${2:-0}" voltage_now="${3:-5000000}"
}
unplug() { w "$C" online=0; [ -d "$U" ] && w "$U" online=0 charger_type= current_max=0 voltage_now=0; }

cat > "$T/i2cget" <<EOS
#!/bin/sh
# i2cget -f -y 1 0x25 REG
cat "$T/reg\$5" 2>/dev/null || exit 1
EOS
chmod +x "$T/i2cget"

# --- charger types (new kernel) ---
fresh
run; check "nothing in" "500 500 auto" "$(lim)"
check "nothing in: LED" none "$(cat "$R/run/zrn-led/charge")"
plug dcp; run; check "dcp" "1800 2100 auto" "$(lim)"
check "dcp: logged" "zrn-charge: wired dcp: in 1800 mA, charge 2100 mA, battery 25.0 C" "$(cat "$T/out")"
check "dcp: LED" charging "$(cat "$R/run/zrn-led/charge")"
run; check "dcp again: nothing logged" "" "$(cat "$T/out")"
plug rp-3a; run; check "rp 3 A" "3000 2150 auto" "$(lim)"
plug rp-1.5a; run; check "rp 1.5 A" "1500 1500 auto" "$(lim)"
plug sdp; run; check "sdp" "500 550 auto" "$(lim)"
plug cdp; run; check "cdp" "1500 1500 auto" "$(lim)"
plug apple-1a; run; check "apple 1 A" "1000 1000 auto" "$(lim)"
plug samsung-2a; run; check "samsung 2 A" "1800 2100 auto" "$(lim)"
plug dcd-timeout; run; check "dcd timeout" "500 500 auto" "$(lim)"
plug ''; w "$U" usb_type='Unknown SDP DCP CDP [C] PD' current_max=3000000; run
check "no charger_type: usb_type C at 3 A" "3000 2150 auto" "$(lim)"
plug pd 3000000 5000000; run; check "pd 5 V 3 A" "3000 2150 auto" "$(lim)"
plug pd 2000000 9000000; run; check "pd 9 V 2 A: 15 W" "1666 2150 auto" "$(lim)"
plug afc-9v 1650000 5000000; run; check "afc, still at 5 V" "1800 2100 auto" "$(lim)"
plug afc-9v 1650000 9000000; run; check "afc 9 V" "1650 2150 auto" "$(lim)"
unplug; tick 10; run; check "unplug" "500 500 auto" "$(lim)"
check "unplug: LED" none "$(cat "$R/run/zrn-led/charge")"

# --- AICL ---
fresh; plug dcp; run
echo 900000 > "$C/input_current_limit"; tick 1; run
check "aicl cut kept" "900 2100 auto" "$(lim)"
check "aicl logged" "zrn-charge: wired dcp: in 900 mA, charge 2100 mA (aicl), battery 25.0 C" "$(cat "$T/out")"
w "$B" temp=260; tick 30; run; check "aicl: not raised on the timer" "900 2100 auto" "$(lim)"
echo 2000000 > "$C/constant_charge_current"; run; check "aicl: charge current still set" "900 2100 auto" "$(lim)"
unplug; tick 1; run; plug dcp; tick 1; run; check "a 1 s drop-out keeps the cut" "900 2100 auto" "$(lim)"
unplug; tick 1; run; tick 10; run; plug dcp; tick 1; run; check "a new plug: stock again" "1800 2100 auto" "$(lim)"
echo 1000000 > "$C/input_current_limit_aicl"; tick 1; run; check "the kernel's aicl attribute" "1000 2100 auto" "$(lim)"
rm "$C/input_current_limit_aicl"
echo 100000 > "$C/input_current_limit"; tick 1; run; check "aicl floor 300 mA" "300 2100 auto" "$(lim)"
echo 3200000 > "$C/input_current_limit"; tick 1; run; check "the old ratchet's wrap to 3.2 A: down again" "300 2100 auto" "$(lim)"
plug rp-3a; tick 1; run; check "another type: the cut is gone" "3000 2150 auto" "$(lim)"

# --- temperature, with hysteresis ---
fresh; plug dcp
t() { w "$B" temp=$1; tick 30; run; check "$1: $2" "$2" "$(lim)"; }
t 250 "1800 2100 auto"
t 410 "1800 1090 auto"
read -r f < "$C/constant_charge_voltage"; check "41 C: float 4.15 V" 4150000 "$f"
t 395 "1800 1090 auto"
t 390 "1800 2100 auto"
read -r f < "$C/constant_charge_voltage"; check "39 C: float back" 4350000 "$f"
t 500 "1800 1090 inhibit-charge"
check "50 C logged" "zrn-charge: wired dcp: in 1800 mA, charge 1090 mA, stopped (warm too hot), battery 50.0 C" "$(cat "$T/out")"
t 460 "1800 1090 inhibit-charge"
t 450 "1800 1090 auto"
t 300 "1800 2100 auto"
t 150 "1800 700 auto"
t 190 "1800 700 auto"
t 200 "1800 2100 auto"
t 50 "1800 700 auto"
read -r f < "$C/constant_charge_voltage"; check "5 C: float 4.15 V" 4150000 "$f"
t 0 "1800 700 inhibit-charge"
t 30 "1800 700 inhibit-charge"
t 50 "1800 700 auto"
t 100 "1800 700 auto"
t 200 "1800 2100 auto"
t -55 "1800 700 inhibit-charge"
check "-5.5 C logged" "zrn-charge: wired dcp: in 1800 mA, charge 700 mA, stopped (cold too cold), battery -5.5 C" "$(cat "$T/out")"
# a float that can't be set: stop at 4.15 V, go on below 4.0 V
fresh; plug dcp; rm "$C/constant_charge_voltage"
w "$B" voltage_now=4200000; t 420 "1800 1090 inhibit-charge"
w "$B" voltage_now=4100000; t 420 "1800 1090 inhibit-charge"
w "$B" voltage_now=3990000; t 420 "1800 1090 auto"
w "$B" voltage_now=4100000; t 420 "1800 1090 auto"
w "$B" voltage_now=4200000; t 380 "1800 2100 auto"
# no charge_behaviour: a stop is the least charge current
fresh; plug dcp; rm "$C/charge_behaviour"; echo '[auto]' > "$C/charge_behaviour.x"
w "$B" temp=520; tick 1; run; check "stop without charge_behaviour" "100" "$(( $(cat "$C/constant_charge_current") / 1000 ))"

# --- 9 V: asked for, prepared, cut, dropped input first ---
fresh; echo 0 > "$U/hv_enable"; plug dcp 1800000 5000000; : > "$T/trace"; run
check "dcp: 9 V asked" 1 "$(cat "$U/hv_enable")"
check "dcp: 500 mA while it changes" "500 500 auto" "$(lim)"
check "dcp: the order" "max77705-charger/input_current_limit 500000
max77705-usbc/hv_enable 1" "$(grep -E 'input_current|hv_enable' "$T/trace")"
plug afc-9v 1650000 9000000; tick 2; run; check "afc 9 V" "1650 2150 auto" "$(lim)"
echo enabled > "$R/sys/class/drm/card0-DSI-1/enabled"; tick 10; run; check "9 V, screen on" "500 1000 auto" "$(lim)"
echo disabled > "$R/sys/class/drm/card0-DSI-1/enabled"; tick 10; run; check "9 V, screen off" "1650 2150 auto" "$(lim)"
w "$B" temp=415; tick 10; : > "$T/trace"; run
check "41 C: input down first, then 9 V off" "max77705-charger/input_current_limit 500000
max77705-usbc/hv_enable 0" "$(grep -E 'input_current|hv_enable' "$T/trace" | head -2)"
check "41 C at 9 V" "500 1090 auto" "$(lim)"
tick 2; run; check "41 C, the bus not yet at 5 V: wait" "500 1090 auto" "$(lim)"
plug dcp 1800000 5000000; tick 2; run; check "41 C back at 5 V" "1800 1090 auto" "$(lim)"
check "41 C: 9 V stays off" 0 "$(cat "$U/hv_enable")"
w "$B" temp=385; tick 30; run; check "39 C: 9 V asked again" 1 "$(cat "$U/hv_enable")"
tick 11; run; check "the source stays at 5 V: dcp after the wait" "1800 2100 auto" "$(lim)"
plug sdp; tick 1; run; check "sdp: no 9 V" 0 "$(cat "$U/hv_enable")"
unplug; tick 1; run; check "unplug: 9 V off" 0 "$(cat "$U/hv_enable")"
fresh; plug dcp; tick 1; run; check "no hv_enable: limits only" "1800 2100 auto" "$(lim)"
[ -e "$U/hv_enable" ] && { echo "FAIL created hv_enable"; fail=1; }

# --- wireless ---
fresh; w "$W" online=1; run
check "wireless" "900 1200" "$(( $(cat "$W/input_current_limit") / 1000 )) $(( $(cat "$C/constant_charge_current") / 1000 ))"
check "wireless: LED" charging "$(cat "$R/run/zrn-led/charge")"
w "$B" temp=420; tick 30; run; check "wireless, warm" "700" "$(( $(cat "$C/constant_charge_current") / 1000 ))"
mkdir -p "$PS/p9320-wireless"; echo 10000000 > "$PS/p9320-wireless/voltage_now"; w "$B" temp=250; tick 30; run
check "wireless 10 V" "650 1200" "$(( $(cat "$W/input_current_limit") / 1000 )) $(( $(cat "$C/constant_charge_current") / 1000 ))"
plug dcp; tick 1; run; check "a cable on the pad: wired wins" "1800 2100 auto" "$(lim)"
# the receiver's 10 V: hv_enable when allowed, its hv_state for the limit
fresh; X=$PS/p9320-wireless; mkdir -p "$X"; w "$X" hv_enable=0 hv_state=5v present=1
wl() { echo "$(( $(cat "$W/input_current_limit") / 1000 )) $(cat "$X/hv_enable")"; }
w "$W" online=1; run; check "pad: 900 mA, 10 V asked" "900 1" "$(wl)"
w "$X" hv_state=prepare; w "$W" input_current_limit=500000; : > "$T/trace"; tick 1; run
check "pad: preparing, WCIN left to the receiver" "" "$(grep wireless/input "$T/trace")"
w "$X" hv_state=10v; tick 1; run; check "pad at 10 V: 650 mA" "650 1" "$(wl)"
w "$B" temp=415; tick 30; run; check "pad, 41 C: 10 V off" "650 0" "$(wl)"
w "$X" hv_state=stepdown; tick 1; run; w "$X" hv_state=5v; tick 1; run
check "pad back at 5 V: 900 mA" "900 0" "$(wl)"
w "$B" temp=250; tick 30; run; check "pad, cool again: 10 V asked" "900 1" "$(wl)"
plug dcp; w "$W" online=0; tick 1; run; check "a cable in: the receiver's 10 V off" 0 "$(cat "$X/hv_enable")"

# --- over-current trips, remembered per charger ---
fresh; w "$C" input_current_trips=0 input_current_trip_limit=0 input_current_trip_voltage=0
plug dcp; run; check "trip: before" "1800 2100 auto" "$(lim)"
w "$C" input_current_trips=1 input_current_trip_limit=1800000 input_current_trip_voltage=5000000; tick 1; run
check "trip: logged loudly" "<3>zrn-charge: CHARGER TRIP: dcp+Unknown cut out at 1800 mA (the charger's over-current count, now 1); from now on it gets at most 1600 mA" "$(head -1 "$T/out")"
check "trip: capped now" "1600 2100 auto" "$(lim)"
tick 30; run; check "trip: logged once" "" "$(cat "$T/out")"
unplug; tick 1; run; tick 10; run; plug dcp; tick 1; run
check "trip: the next plug of that charger" "1600 2100 auto" "$(lim)"
check "trip: says why" "zrn-charge: wired dcp: in 1600 mA, charge 2100 mA (tripped at 1800 before), battery 25.0 C" "$(cat "$T/out")"
tick 30; run; tick 61; run
check "--show" "dcp+Unknown: last seen $(date -d @$EP +%F), held 1600 mA, tripped at 1800 mA (now at most 1600 mA)" \
	"$(ZRN_CHARGE_ROOT=$R ZRN_CHARGE_EPOCH=$EP bash "$Z" --show)"
plug rp-3a; unplug; tick 1; run; tick 10; run; plug rp-3a; tick 1; run
check "trip: another charger is not capped" "3000 2150 auto" "$(lim)"
check "two remembered" 2 "$(ZRN_CHARGE_ROOT=$R ZRN_CHARGE_EPOCH=$EP bash "$Z" --show | wc -l)"
ZRN_CHARGE_ROOT=$R bash "$Z" --forget dcp+Unknown
check "--forget one" "rp-3a+Unknown" "$(ZRN_CHARGE_ROOT=$R ZRN_CHARGE_EPOCH=$EP bash "$Z" --show | cut -d: -f1)"
ZRN_CHARGE_ROOT=$R bash "$Z" --forget nonesuch 2>/dev/null; check "--forget unknown fails" 1 $?
ZRN_CHARGE_ROOT=$R bash "$Z" --forget all
check "--forget all" "zrn-charge: no charger remembered" "$(ZRN_CHARGE_ROOT=$R ZRN_CHARGE_EPOCH=$EP bash "$Z" --show)"
# ageing: 31 days later the charger is new again
unplug; tick 1; run; tick 10; run; plug dcp; tick 1; run
w "$C" input_current_trips=2; tick 1; run; check "trip again" "1600 2100 auto" "$(lim)"
unplug; tick 1; run; tick 10; run; EP=$((EP + 31 * 86400)); plug dcp; tick 1; run
check "aged out after 30 days" "1800 2100 auto" "$(lim)"
EP=1800000000
# a trip at 9 V: 5 V only for that charger, the limit down first
fresh; w "$C" input_current_trips=0; echo 1 > "$U/hv_enable"; plug afc-9v 1650000 9000000; run
w "$C" input_current_trips=1 input_current_trip_limit=1650000 input_current_trip_voltage=9000000; : > "$T/trace"; tick 1; run
check "9 V trip: logged" "<3>zrn-charge: CHARGER TRIP: afc-9v+Unknown cut out at 1650 mA at 9 V (the charger's over-current count, now 1); from now on it gets at most 1450 mA, and 5 V only" "$(head -1 "$T/out")"
check "9 V trip: the limit down, then 9 V off" "max77705-charger/input_current_limit 500000
max77705-usbc/hv_enable 0" "$(grep -E 'input_current|hv_enable' "$T/trace" | head -2)"
unplug; tick 1; run; tick 10; run; plug afc-9v 1650000 5000000; tick 1; run
check "9 V trip: the next plug stays at 5 V" "0 1450 2100 auto" "$(cat "$U/hv_enable") $(lim)"
# today's kernel: CHGIN gone and back within 5 s at 1 A or more is a trip
fresh old; echo 0x8b > "$T/reg0x08"; echo 0x00 > "$T/reg0x0a"
w "$C" online=1; run; w "$C" online=0; tick 1; run; w "$C" online=1; tick 2; run
check "old: a drop-out at 1.8 A is a trip" "<3>zrn-charge: CHARGER TRIP: dcp cut out at 1800 mA (the input dropped out for 2 s); from now on it gets at most 1600 mA" "$(head -1 "$T/out")"
check "old: capped" "1600 2100 auto" "$(lim)"

# --- PD: caps changes followed, Hard Resets logged, the anomaly ---
pd() {	# caps, current_max uA, voltage uV
	w "$U" pd_source_caps="$1" current_max="$2" voltage_now="${3:-5000000}"; tick 1; run
}
fresh; w "$U" pd_hard_resets=0 pd_caps_reduced_after_hard_reset=0; echo 0 > "$U/hv_enable"
w "$C" online=1; w "$U" online=1 charger_type=pd usb_type='Unknown SDP DCP CDP C [PD]'
pd "5000:3000 9000:2000" 3000000; pd "5000:3000 9000:2000" 2000000 9000000
check "pd 9 V" "1666 2150 auto" "$(lim)"
pd "5000:3000 9000:1500" 1500000 9000000
check "pd: other caps, no reset: followed" "1500 2150 auto" "$(lim)"
check "pd: other caps: one info line" "zrn-charge: PD source caps changed, followed: 5000:3000 9000:2000 -> 5000:3000 9000:1500" "$(head -1 "$T/out")"
check "pd: the fingerprint ignores currents" "pd+PD+5V,9V" "$(cut -d' ' -f6 "$R"/var/lib/zrn-charge/*)"
check "pd: no penalty remembered" "0 0 0 0" "$(cut -d' ' -f1-4 "$R"/var/lib/zrn-charge/*)"
w "$U" pd_hard_resets=1; pd "5000:3000 9000:1500" 1500000 9000000
check "pd: a Hard Reset, logged" "<4>zrn-charge: PD Hard Reset (number 1 on this plug): pd+PD+5V,9V at 9 V, in 1500 mA, caps 5000:3000 9000:1500" "$(head -1 "$T/out")"
check "pd: one Hard Reset, same caps: nothing else" "1500 2150 auto 1" "$(lim) $(cat "$U/hv_enable")"
w "$U" pd_hard_resets=2; : > "$T/trace"; pd "5000:3000 9000:1500" 1500000 9000000
check "pd: a second Hard Reset: 5 V, the limit down first" "max77705-charger/input_current_limit 500000
max77705-usbc/hv_enable 0" "$(grep -E 'input_current|hv_enable' "$T/trace" | head -2)"
pd "5000:3000 9000:1500" 1500000 5000000; check "pd: then 5 V, 1500 mA" "1500 2150 auto 0" "$(lim) $(cat "$U/hv_enable")"
check "pd: a second reset is not remembered" "0" "$(cut -d' ' -f4 "$R"/var/lib/zrn-charge/*)"
# a Hard Reset, then other caps: the anomaly, remembered
unplug; tick 1; run; tick 10; run
w "$C" online=1; w "$U" online=1 charger_type=pd; pd "5000:3000 9000:2000" 3000000
w "$U" pd_hard_resets=3 pd_caps_reduced_after_hard_reset=1; pd "5000:1200 9000:1000" 1200000
check "pd anomaly: logged" "<4>zrn-charge: PD ANOMALY, worth investigating: pd+PD+5V,9V: other source caps after a Hard Reset (before: 5000:3000 9000:2000; after: 5000:1200 9000:1000); 5 V and at most 1200 mA until it is unplugged" "$(sed -n 2p "$T/out")"
check "pd anomaly: 5 V, the caps' 5 V current" "1200 2150 auto 0" "$(lim) $(cat "$U/hv_enable")"
check "pd anomaly: remembered" "1" "$(cut -d' ' -f4 "$R"/var/lib/zrn-charge/*)"
unplug; tick 1; run; tick 10; run
w "$C" online=1; w "$U" online=1 charger_type=pd; pd "5000:3000 9000:2000" 3000000
check "pd anomaly: the next plug, 5 V only" "3000 2150 auto 0" "$(lim) $(cat "$U/hv_enable")"
# without the driver's count: the caps seen to change after the reset
fresh; w "$U" pd_hard_resets=0; w "$C" online=1; w "$U" online=1 charger_type=pd
pd "5000:3000 9000:2000" 3000000; w "$U" pd_hard_resets=1; pd "5000:3000 9000:2000" 3000000
pd "5000:1500" 1500000; check "pd: no count, other caps after a reset: the anomaly" "1500 2150 auto" "$(lim)"
grep -q 'PD ANOMALY' "$T/out" || { echo "FAIL no anomaly logged"; fail=1; }

# --- low battery: a PD charger with 5 V x 3 A stays at 5 V, without flapping ---
fresh; w "$U" pd_hard_resets=0; echo 0 > "$U/hv_enable"; w "$B" capacity=10
w "$C" online=1; w "$U" online=1 charger_type=pd usb_type='Unknown SDP DCP CDP C [PD]'
pd "5000:3000 9000:3000" 3000000
check "low: 10 %, 5 V x 3 A: stays at 5 V" "0" "$(cat "$U/hv_enable")"
grep -q '5 V while under 15 %' "$T/out" || { echo "FAIL low: no reason logged"; fail=1; }
w "$B" capacity=14; pd "5000:3000 9000:3000" 3000000
check "low: 14 %: still 5 V" "0" "$(cat "$U/hv_enable")"
w "$B" capacity=15; pd "5000:3000 9000:3000" 3000000
check "low: 15 %: up to 9 V" "1" "$(cat "$U/hv_enable")"
w "$B" capacity=14; pd "5000:3000 9000:3000" 3000000 9000000
check "low: back at 14 % on 9 V: stays (no flapping)" "1" "$(cat "$U/hv_enable")"
w "$B" capacity=12; pd "5000:3000 9000:3000" 3000000 5000000
check "low: let go once, a 5 V moment at 12 % doesn't bring it back" "1" "$(cat "$U/hv_enable")"
unplug; tick 1; run; tick 10; run
w "$C" online=1; w "$U" online=1 charger_type=pd; w "$B" capacity=12; pd "5000:3000 9000:3000" 3000000
check "low: a new plug at 12 %: held again" "0" "$(cat "$U/hv_enable")"
fresh; w "$B" capacity=10; echo 0 > "$U/hv_enable"; w "$C" online=1; w "$U" online=1 charger_type=pd
pd "5000:2000 9000:2000" 2000000
check "low: 5 V only 2 A: 9 V still allowed at 10 %" "1" "$(cat "$U/hv_enable")"
w "$B" capacity=50

# --- today's kernel: no max77705-usbc, the MUIC over I2C ---
fresh old
reg() { echo "$1" > "$T/reg0x08"; echo "${2:-0x00}" > "$T/reg0x0a"; tick 1; run; }
w "$C" online=1
reg 0x8b; check "old: dcp (8b)" "1800 2100 auto" "$(lim)"
reg 0x83; check "old: dcp (83)" "1800 2100 auto" "$(lim)"
reg 0x82; check "old: cdp" "1500 1500 auto" "$(lim)"
reg 0x81; check "old: sdp" "500 550 auto" "$(lim)"
reg 0x85; check "old: dcd timeout" "500 500 auto" "$(lim)"
reg 0x89; check "old: samsung 2 A on sdp" "1800 2100 auto" "$(lim)"
reg 0x99; check "old: apple 1 A" "1000 1000 auto" "$(lim)"
reg 0xb3; check "old: dcp 3 A" "1800 2100 auto" "$(lim)"
reg 0x85 0x30; check "old: dcd timeout, Rp 3 A" "3000 2150 auto" "$(lim)"
reg 0x81 0x20; check "old: sdp, Rp 1.5 A" "1500 1500 auto" "$(lim)"
reg 0x83 0x20; check "old: dcp, Rp 1.5 A: dcp" "1800 2100 auto" "$(lim)"
reg 0x82 0x30; check "old: cdp, Rp 3 A: cdp (as stock)" "1500 1500 auto" "$(lim)"
reg 0x80; check "old: not yet known" "500 500 auto" "$(lim)"
rm -f "$T/reg0x08" "$T/reg0x0a"; tick 1; run; check "old: no i2cget answer" "500 500 auto" "$(lim)"
w "$B" temp=505; reg 0x8b; check "old: hot" "1800 1090 inhibit-charge" "$(lim)"
w "$C" online=0; w "$B" temp=250; tick 10; run; check "old: unplug" "500 500 auto" "$(lim)"

# --- the LED, as it was ---
fresh
led() {	# status capacity online wanted
	w "$B" status="$1" capacity="$2"; w "$C" online="$3"; w "$U" online="$3" charger_type=sdp; run
	check "LED $1 $2% online=$3" "$4" "$(cat "$R/run/zrn-led/charge")"
}
led Charging 50 1 charging
led Full 100 1 full
led Discharging 50 0 none
led Discharging 15 0 low
led Discharging 3 1 low
led Discharging 50 1 charging
led "Not charging" 96 1 full
led "Not charging" 50 1 none
led "Not charging" 10 0 low
mv "$R/run/zrn-led" "$R/run/away"; run
check "no zrn-ledd: quiet" "" "$(cat "$T/out")"

# --- the kernel's ramp: the input limit is a ceiling, written once per decision ---
fresh; w "$C" input_current_ramp=ramping input_current_limit_effective=500000 input_current_limit_found=0
plug dcp; : > "$T/trace"; run
check "ramp: the dcp ceiling" "1800 2100 auto" "$(lim)"
check "ramp: logged" "zrn-charge: wired dcp: in 1800 mA, charge 2100 mA, ramp ramping, battery 25.0 C, effective 500 mA, found 0 mA" "$(cat "$T/out")"
# the kernel holds 1200 mA (and, say, reads that back): not "corrected"
w "$C" input_current_limit=1200000 input_current_ramp=held input_current_limit_effective=1200000 input_current_limit_found=1200000
: > "$T/trace"; tick 30; run
check "ramp: not written again" "" "$(grep input_current "$T/trace")"
check "ramp: the held ramp, one line" "zrn-charge: wired dcp: in 1800 mA, charge 2100 mA, ramp held, battery 25.0 C, effective 1200 mA, found 1200 mA" "$(cat "$T/out")"
tick 30; run; check "ramp: then quiet" "" "$(cat "$T/out")"
w "$B" temp=420; : > "$T/trace"; tick 30; run
check "ramp: warm, the charge current only" "max77705-charger/constant_charge_current 1090000" "$(grep current "$T/trace")"
plug rp-3a; : > "$T/trace"; tick 1; run
check "ramp: another type, one write" "max77705-charger/input_current_limit 3000000" "$(grep input_current "$T/trace")"
unplug; : > "$T/trace"; tick 1; run; tick 1; run
check "ramp: unplugged, one write" "max77705-charger/input_current_limit 500000" "$(grep input_current "$T/trace")"

# --- the daemon ---
fresh; mkfifo "$T/ev"
ZRN_CHARGE_ROOT=$R ZRN_CHARGE_MONITOR="cat $T/ev" bash "$Z" --daemon > "$T/dlog" 2>&1 &
d=$!
exec 4> "$T/ev"
wait_for() {	# wanted lim, within 5 s
	local i
	for i in $(seq 50); do [ "$(lim)" = "$1" ] && return 0; sleep 0.1; done
	return 1
}
sleep 0.5; plug dcp; echo "KERNEL change max77705-usbc" >&4
wait_for "1800 2100 auto"; check "daemon: dcp" "1800 2100 auto" "$(lim)"
for i in $(seq 40); do echo "KERNEL change max170xx_battery" >&4; done; sleep 1.5
check "daemon: a burst, one line" 1 "$(grep -c 'wired dcp' "$T/dlog")"
plug rp-3a; ZRN_CHARGE_ROOT=$R bash "$Z"	# a lone call: the daemon makes the pass
wait_for "3000 2150 auto"; check "daemon: asked by a lone call" "3000 2150 auto" "$(lim)"
kill -TERM $d; wait $d; check "daemon: TERM is a clean exit" 0 $?
[ -e "$R/run/zrn-charge/pid" ] && { echo "FAIL pid file left"; fail=1; }
exec 4>&-
ZRN_CHARGE_ROOT=$R ZRN_CHARGE_MONITOR="true" bash "$Z" --daemon > /dev/null 2>&1
check "daemon: the monitor gone is a failure (restart)" 1 $?

[ $fail = 0 ] && echo "zrn-charge: all ok"
exit $fail
