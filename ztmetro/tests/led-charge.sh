#!/bin/sh
# make check-led: zrn-charge says what the battery is doing, as it used to draw
# it, and writes no LED. Its charging limits are stock's. It is run against a
# fake sysfs (ZRN_CHARGE_ROOT) with today's kernel (the MUIC over a fake
# i2cget); devices/samsung-starlte/test-zrn-charge.sh tests the policy in full.
SRC=$(readlink -f "$1")
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "led-charge: FAIL: $*"; exit 1; }
if grep -q 'class/leds' "$SRC"; then fail "zrn-charge still writes the LED"; fi

R=$T/root
C=$R/sys/class/power_supply/max77705-charger B=$R/sys/class/power_supply/max170xx_battery
mkdir -p "$C" "$B" "$R/run/zrn-led" "$R/proc"
echo "100.00 100.00" > "$R/proc/uptime"
cat > "$T/i2cget" <<EOS
#!/bin/sh
[ "\$5" = 0x08 ] && cat "$T/bc" || echo 0x00
EOS
chmod +x "$T/i2cget"
run() {	# status capacity online bc
	echo "$1" > "$B/status"; echo "$2" > "$B/capacity"; echo "$3" > "$C/online"; echo "$4" > "$T/bc"
	ZRN_CHARGE_ROOT=$R ZRN_CHARGE_I2CGET=$T/i2cget ZRN_CHARGE_DETECT_S=0 sh "$SRC" > /dev/null
}
claim() { cat "$R/run/zrn-led/charge"; }
limits() { echo "$(cat "$C/input_current_limit") $(cat "$C/constant_charge_current")"; }
echo 500000 > "$C/input_current_limit"; echo 450000 > "$C/constant_charge_current"
want() {	# status capacity online claim
	run "$1" "$2" "$3" 0x00
	[ "$(claim)" = "$4" ] || fail "$1 $2% online=$3: claims $(claim), wanted $4"
}
want Charging 50 1 charging
want Charging 5 1 charging
want Full 100 1 full
want Discharging 50 0 none
want Discharging 16 0 none
want Discharging 15 0 low
want Discharging 3 1 low		# a computer's port that gives too little
want Discharging 50 1 charging	# the same port, with charge to spare: a cable in is a light on
want "Not charging" 96 1 full
want "Not charging" 50 1 none
want "Not charging" 10 0 low
want Unknown 10 1 none

# only when it changes: zrn-ledd wakes for each write
want Discharging 50 0 none
a=$(stat -c %y "$R/run/zrn-led/charge"); sleep 0.05
want Discharging 49 0 none
[ "$a" = "$(stat -c %y "$R/run/zrn-led/charge")" ] || fail "wrote the same claim again"

# without zrn-ledd's directory it says nothing and goes on to the limits
mv "$R/run/zrn-led" "$R/run/away"
echo 1500000 > "$C/input_current_limit"
run Discharging 50 0 0x00 2>"$T/err" || fail "stopped without the directory"
[ ! -s "$T/err" ] || fail "complained: $(cat "$T/err")"
[ "$(cat "$C/input_current_limit")" = 500000 ] || fail "limit not set without zrn-ledd"
mv "$R/run/away" "$R/run/zrn-led"

# the limits, stock's: wall charger, nothing, charging port, computer
run Charging 50 1 0x83; [ "$(limits)" = "1800000 2100000" ] || fail "wall charger: $(limits)"
run Charging 50 0 0x00; [ "$(limits)" = "500000 500000" ] || fail "unplugged: $(limits)"
echo "200.00 0" > "$R/proc/uptime"	# later: a new plug
run Charging 50 1 0x82; [ "$(limits)" = "1500000 1500000" ] || fail "charging port: $(limits)"
run Charging 50 1 0x81; [ "$(limits)" = "500000 550000" ] || fail "computer: $(limits)"
echo "led-charge: ok"
