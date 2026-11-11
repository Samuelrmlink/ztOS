#!/bin/sh
# Move Apps (ztmove): running apps from one side to the other, without a
# restart, from an app that is on the phone or on the desktop: run inside
# desktop/dev/run.sh
#
#   ztmetro/desktop/dev/run.sh --test mover
set -u
OUT=HEADLESS-2
OX=360
pass=0 fail=0
c() { echo "$*" > "$XDG_RUNTIME_DIR/ztmetro.ctl"; }
poke() { ./desktop/zt-deskpoke "$OUT" 1600 900 "$*"; }
ok() { pass=$((pass + 1)); echo "PASS $*"; }
no() { fail=$((fail + 1)); echo "FAIL $*"; }
check() { if eval "$2"; then ok "$1"; else no "$1 ($3)"; fi; }
win() { c desktop state; sleep 0.3; grep -E "^window [a-z0-9]+ $1 " "$XDG_RUNTIME_DIR/ztmetro-desktop.state" | head -1; }
waitfor() { i=0; while [ $i -lt $(($1 * 10)) ]; do eval "$2" && return 0; sleep 0.1; i=$((i + 1)); done; return 1; }

# a terminal on the phone (started from no desktop session, nothing on the desktop lately)
foot -a footphone >/dev/null 2>&1 &
fpid=$!
waitfor 6 'grep -q footphone $XDG_RUNTIME_DIR/ztmetro-apps'
check "foot opened on the phone" "grep -q footphone $XDG_RUNTIME_DIR/ztmetro-apps" "$(cat $XDG_RUNTIME_DIR/ztmetro-apps)"

# Move Apps on the desktop (as XFCE's menu starts it)
ZT_DESKTOP=1 ZTMOVE_TRACE=1 ./desktop/ztmove > /tmp/move.log 2>&1 &
mpid=$!
waitfor 6 'grep -q "page on the desktop" /tmp/move.log'
waitfor 4 'grep -q "row bring footphone" /tmp/move.log'
check "on the desktop it lists the phone's apps, to bring here" "grep -q 'row bring footphone' /tmp/move.log" \
	"$(tail -3 /tmp/move.log)"
line=$(win ztmove)
x=$(echo "$line" | awk '{print $4 - '$OX'}'); y=$(echo "$line" | awk '{print $5}')
row=$(grep 'row bring footphone' /tmp/move.log | tail -1)
ry=$(echo "$row" | awk '{print int(($5 + $6) / 2)}')
grim -o "$OUT" /out/mover-desktop.png
poke "click $((x + 5 + 120)) $((y + 29 + ry))"
waitfor 4 '[ -n "$(win footphone)" ]'
check "a click brings foot to the desktop" "[ -n \"\$(win footphone)\" ]" "$(grep window $XDG_RUNTIME_DIR/ztmetro-desktop.state)"
check "the same foot (not restarted)" "kill -0 $fpid" "gone"
waitfor 4 'grep -q "row send footphone" /tmp/move.log'
check "and the list now offers to send it back" "grep -q 'row send footphone' /tmp/move.log" "$(tail -3 /tmp/move.log)"
grim -o "$OUT" /out/mover-desktop-after.png

# the row that sends it back (foot came to the front: out of the way first)
fl=$(win footphone)
fx=$(echo "$fl" | awk '{print $4 - '$OX'}'); fy=$(echo "$fl" | awk '{print $5}')
poke "drag $((fx + 300)) $((fy + 14)) 1450 820 300 10"
sleep 0.4
row=$(grep 'row send footphone' /tmp/move.log | tail -1)
ry=$(echo "$row" | awk '{print int(($5 + $6) / 2)}')
line=$(win ztmove)
x=$(echo "$line" | awk '{print $4 - '$OX'}'); y=$(echo "$line" | awk '{print $5}')
poke "click $((x + 5 + 120)) $((y + 29 + ry))"
waitfor 4 '[ -z "$(win footphone)" ]'
check "a click sends it back to the phone" "[ -z \"\$(win footphone)\" ] && grep -q footphone $XDG_RUNTIME_DIR/ztmetro-apps" \
	"$(grep window $XDG_RUNTIME_DIR/ztmetro-desktop.state)"
check "still the same foot" "kill -0 $fpid" "gone"

# Move Apps itself to the phone: it turns round
c desktop move footphone
waitfor 4 '[ -n "$(win footphone)" ]'
: > /tmp/move.log
c desktop phone ztmove
waitfor 6 'grep -q "page on the phone" /tmp/move.log'
waitfor 4 'grep -q "row bring footphone" /tmp/move.log'
check "on the phone it lists the desktop's windows, to bring here" \
	"grep -q 'page on the phone' /tmp/move.log && grep -q 'row bring footphone' /tmp/move.log" "$(tail -3 /tmp/move.log)"
check "the same Move Apps (it moved too)" "kill -0 $mpid" "gone"
grim -o HEADLESS-1 /out/mover-phone.png
kill $fpid $mpid 2>/dev/null
echo "mover: $pass passed, $fail failed"
