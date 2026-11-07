#!/bin/sh
# make check-led: zrn-charge says what the battery is doing, as it used to draw
# it, and writes no LED. Its charging limits are as they were. A copy is run with
# sysfs and I2C replaced.
SRC=$(readlink -f "$1")
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "led-charge: FAIL: $*"; exit 1; }
if grep -q 'class/leds' "$SRC"; then fail "zrn-charge still writes the LED"; fi

C=$T/ps/max77705-charger B=$T/ps/max170xx_battery
mkdir -p "$C" "$B" "$T/run/zrn-led" "$T/bin"
sed -e "s|/sys/class/power_supply|$T/ps|g" -e "s|/run/zrn-led|$T/run/zrn-led|g" "$SRC" > "$T/zrn-charge"
cat > "$T/bin/i2cget" <<EOS
#!/bin/sh
cat "$T/bc"
EOS
chmod +x "$T/bin/i2cget"
run() {	# status capacity online bc
	echo "$1" > "$B/status"; echo "$2" > "$B/capacity"; echo "$3" > "$C/online"; echo "$4" > "$T/bc"
	PATH="$T/bin:$PATH" sh "$T/zrn-charge" > /dev/null
}
claim() { cat "$T/run/zrn-led/charge"; }
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
want "Not charging" 96 1 full
want "Not charging" 50 1 none
want "Not charging" 10 0 low
want Unknown 10 1 none

# only when it changes: zrn-ledd wakes for each write
want Discharging 50 0 none
a=$(stat -c %y "$T/run/zrn-led/charge"); sleep 0.05
want Discharging 49 0 none
[ "$a" = "$(stat -c %y "$T/run/zrn-led/charge")" ] || fail "wrote the same claim again"

# without zrn-ledd's directory it says nothing and goes on to the limits
mv "$T/run/zrn-led" "$T/run/away"
echo 1500000 > "$C/input_current_limit"
run Discharging 50 0 0x00 2>"$T/err" || fail "stopped without the directory"
[ ! -s "$T/err" ] || fail "complained: $(cat "$T/err")"
[ "$(cat "$C/input_current_limit")" = 500000 ] || fail "limit not set without zrn-ledd"
mv "$T/run/away" "$T/run/zrn-led"

# the limits, as ever: wall charger, nothing, charging port, computer
echo 500000 > "$C/input_current_limit"; echo 450000 > "$C/constant_charge_current"
run Charging 50 1 0x83; [ "$(limits)" = "1500000 1500000" ] || fail "wall charger: $(limits)"
run Charging 50 1 0x00; [ "$(limits)" = "500000 450000" ] || fail "unplugged: $(limits)"
run Charging 50 1 0x82; [ "$(limits)" = "1500000 1500000" ] || fail "charging port: $(limits)"
run Charging 50 1 0x81; [ "$(limits)" = "500000 450000" ] || fail "computer: $(limits)"
echo "led-charge: ok"
