#!/bin/sh
# Move Apps (ztmove): running apps from one side to the other, without a
# restart, from an app that is on the phone or on the desktop: run inside
# desktop/dev/run.sh
#
#   ztmetro/desktop/dev/run.sh --test mover
set -u
. "$(dirname "$0")/testlib.sh"
win() { c desktop state; sleep 0.3; grep -E "^window [a-z0-9]+ $1 " "$XDG_RUNTIME_DIR/ztmetro-desktop.state" | head -1; }

# a terminal on the phone (started from no desktop session, nothing on the desktop lately)
$PHONE_CMD >/dev/null 2>&1 &
fpid=$!
waitfor 6 'grep -q $PHONE_ID $XDG_RUNTIME_DIR/ztmetro-apps'
check "foot opened on the phone" "grep -q $PHONE_ID $XDG_RUNTIME_DIR/ztmetro-apps" "$(cat $XDG_RUNTIME_DIR/ztmetro-apps)"

# Move Apps on the desktop (as XFCE's menu starts it)
ZT_DESKTOP=1 ZTMOVE_TRACE=1 "$MOVE" > /tmp/move.log 2>&1 &
mpid=$!
waitfor 6 'grep -q "page on the desktop" /tmp/move.log'
waitfor 4 'grep -q "row bring $PHONE_ID" /tmp/move.log'
check "on the desktop it lists the phone's apps, to bring here" "grep -q 'row bring $PHONE_ID' /tmp/move.log" \
	"$(tail -3 /tmp/move.log)"
line=$(win ztmove)
x=$(echo "$line" | awk '{print $4 - '$OX'}'); y=$(echo "$line" | awk '{print $5}')
row=$(grep "row bring $PHONE_ID" /tmp/move.log | tail -1)
ry=$(echo "$row" | awk '{print int(($5 + $6) / 2)}')
grim -o "$OUT" $OUTDIR/mover-desktop.png
poke "click $((x + 5 + 120)) $((y + 29 + ry))"
waitfor 4 '[ -n "$(win $PHONE_ID)" ]'
check "a click brings foot to the desktop" "[ -n \"\$(win $PHONE_ID)\" ]" "$(grep window $XDG_RUNTIME_DIR/ztmetro-desktop.state)"
check "the same foot (not restarted)" "kill -0 $fpid" "gone"
waitfor 4 'grep -q "row send $PHONE_ID" /tmp/move.log'
check "and the list now offers to send it back" "grep -q 'row send $PHONE_ID' /tmp/move.log" "$(tail -3 /tmp/move.log)"
grim -o "$OUT" $OUTDIR/mover-desktop-after.png

# the row that sends it back (foot came to the front: out of the way first)
fl=$(win $PHONE_ID)
fx=$(echo "$fl" | awk '{print $4 - '$OX'}'); fy=$(echo "$fl" | awk '{print $5}')
poke "drag $((fx + 300)) $((fy + 14)) 1450 820 300 10"
sleep 0.4
row=$(grep "row send $PHONE_ID" /tmp/move.log | tail -1)
ry=$(echo "$row" | awk '{print int(($5 + $6) / 2)}')
line=$(win ztmove)
x=$(echo "$line" | awk '{print $4 - '$OX'}'); y=$(echo "$line" | awk '{print $5}')
poke "click $((x + 5 + 120)) $((y + 29 + ry))"
waitfor 4 '[ -z "$(win $PHONE_ID)" ]'
check "a click sends it back to the phone" "[ -z \"\$(win $PHONE_ID)\" ] && grep -q $PHONE_ID $XDG_RUNTIME_DIR/ztmetro-apps" \
	"$(grep window $XDG_RUNTIME_DIR/ztmetro-desktop.state)"
check "still the same foot" "kill -0 $fpid" "gone"

# Move Apps itself to the phone: it turns round
c desktop move $PHONE_ID
waitfor 4 '[ -n "$(win $PHONE_ID)" ]'
: > /tmp/move.log
c desktop phone ztmove
waitfor 6 'grep -q "page on the phone" /tmp/move.log'
waitfor 4 'grep -q "row bring $PHONE_ID" /tmp/move.log'
check "on the phone it lists the desktop's windows, to bring here" \
	"grep -q 'page on the phone' /tmp/move.log && grep -q 'row bring $PHONE_ID' /tmp/move.log" "$(tail -3 /tmp/move.log)"
check "the same Move Apps (it moved too)" "kill -0 $mpid" "gone"
grim -o "$PANEL" $OUTDIR/mover-phone.png
kill $fpid $mpid 2>/dev/null
echo "mover: $pass passed, $fail failed"
