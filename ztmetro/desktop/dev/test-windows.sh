#!/bin/sh
# Window management on the desktop, checked: run inside desktop/dev/run.sh
#
#   ztmetro/desktop/dev/run.sh 'sh desktop/dev/test-windows.sh'
#
# Every check prints PASS or FAIL and the summary is the last line. The
# pointer is driven the way ztdesk drives it (zt-deskpoke: a virtual pointer
# on the desktop's output), so these are the paths the phone takes.
set -u
. "$(dirname "$0")/testlib.sh"
shot() { grim -o "$OUT" "$OUTDIR/$1-desktop.png"; }
# the window line of APP (by app_id or title), fields: kind id x y w h flags...
win() { c desktop state; sleep 0.3; grep -E "^window [a-z0-9]+ [^ ]*$1" "$XDG_RUNTIME_DIR/ztmetro-desktop.state" | head -1; }
wx() { win "$1" | awk '{print $4 - '$OX'}'; }
wy() { win "$1" | awk '{print $5}'; }
ww() { win "$1" | awk '{print $6}'; }
wh() { win "$1" | awk '{print $7}'; }
flags() { win "$1" | awk '{print $8}'; }
# a pixel of the desktop, as rrggbb
pixel() {
	grim -o "$OUT" -t ppm /tmp/px.ppm 2>/dev/null
	python3 - "$1" "$2" <<'EOF'
import sys
x, y = int(sys.argv[1]), int(sys.argv[2])
d = open('/tmp/px.ppm', 'rb').read()
parts, i = [], 0
while len(parts) < 4:
    while d[i:i+1].isspace(): i += 1
    j = i
    while not d[j:j+1].isspace(): j += 1
    parts.append(d[i:j]); i = j
i += 1
w = int(parts[1])
o = i + (y * w + x) * 3
print('%02x%02x%02x' % (d[o], d[o+1], d[o+2]))
EOF
}
near() { # near A B TOL: two numbers within TOL
	[ "$(awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN { d = a - b; if (d < 0) d = -d; print (d <= t) ? 1 : 0 }')" = 1 ]
}

theme0=$(xfconf-query -c xfwm4 -p /general/theme 2>/dev/null)	# put back at the end
ZT_DESKTOP=1 xfce4-terminal --disable-server --title=csd >/dev/null 2>&1 &
csd_pid=$!
ZT_DESKTOP=1 $SSD_CMD >/dev/null 2>&1 &
ssd_pid=$!
sleep 4
echo "windows: $(win xfce4-terminal) | $(win $SSD_ID)"

# --- move: CSD (GTK's header bar asks, xdg_toplevel.move), fast as a flick
x0=$(wx xfce4-terminal); y0=$(wy xfce4-terminal)
poke "drag $((x0 + 150)) $((y0 + 17)) $((x0 - 174)) $((y0 + 328)) 100 5"
sleep 0.5
x1=$(wx xfce4-terminal); y1=$(wy xfce4-terminal)
check "csd: a fast header-bar drag moves the window all the way" \
	"near $((x1 - x0)) -324 2 && near $((y1 - y0)) 311 2" "moved $((x1 - x0)),$((y1 - y0)), wanted -324,311"
# after the drag nothing follows the pointer any more
poke "move 1500 850; sleep 200; move 1400 800"
sleep 0.3
check "csd: the window stays put after the drag" "[ $(wx xfce4-terminal) = $x1 ] && [ $(wy xfce4-terminal) = $y1 ]" \
	"now $(wx xfce4-terminal),$(wy xfce4-terminal)"

# --- resize: CSD (GTK asks, xdg_toplevel.resize, from its own border)
x0=$(wx xfce4-terminal); y0=$(wy xfce4-terminal); w0=$(ww xfce4-terminal)
poke "click $((x0 + 100)) $((y0 + 300))"
poke "drag $((x0 + w0 + 2)) $((y0 + 200)) $((x0 + w0 + 82)) $((y0 + 200)) 300 10"
sleep 0.5
w1=$(ww xfce4-terminal)
check "csd: dragging GTK's border makes it wider" "[ $w1 -gt $((w0 + 40)) ]" "width $w0 -> $w1"

# --- move: our title bar (foot asks for server-side decorations)
c desktop pointer 10 890
# (to 300,100 on the output: wherever it opened, its edges are clear of the panel then)
x0=$(wx $SSD_ID); y0=$(wy $SSD_ID)
poke "drag $((x0 + 200)) $((y0 + 14)) 500 114 300 10"
sleep 0.4
x1=$(wx $SSD_ID); y1=$(wy $SSD_ID)
check "ssd: a title-bar drag moves the window" "near $((x1 - x0)) $((300 - x0)) 2 && near $((y1 - y0)) $((100 - y0)) 2" \
	"moved $((x1 - x0)),$((y1 - y0))"

# --- the frame's colours: xfwm4's Default on Adwaita, not the phone's accent
pix=$(pixel $((x1 + 30)) $((y1 + 3)))
check "ssd: the focused title bar is xfwm4's Default on Adwaita (d8d5d2), not the accent" "[ $pix = d8d5d2 ]" "pixel $pix"
pix=$(pixel $((x1 + 2)) $((y1 + 100)))
check "ssd: the left side is drawn (f6f5f4)" "[ $pix = f6f5f4 ]" "pixel $pix"

# --- resize: the side (inside the frame) and the margin outside, over xfdesktop
w0=$(ww $SSD_ID); h0=$(wh $SSD_ID)
fw=$((w0 + 10)); fh=$((h0 + 29 + 5))
poke "drag $((x1 + fw - 2)) $((y1 + 200)) $((x1 + fw + 98)) $((y1 + 200)) 300 10"
sleep 0.5
w1=$(ww $SSD_ID)
check "ssd: dragging the right side makes it wider" "[ $w1 -gt $((w0 + 80)) ]" "width $w0 -> $w1"
h0=$(wh $SSD_ID); fh=$((h0 + 29 + 5))
poke "drag $((x1 + 300)) $((y1 + fh + 3)) $((x1 + 300)) $((y1 + fh + 63)) 300 10"
sleep 0.5
h1=$(wh $SSD_ID)
check "ssd: dragging just below it (over the desktop's background) makes it taller" "[ $h1 -gt $((h0 + 40)) ]" \
	"height $h0 -> $h1"

# --- the buttons: they act on release, over them
x0=$(wx $SSD_ID); y0=$(wy $SSD_ID); fw=$(($(ww $SSD_ID) + 10))
# Default's right group: shade, hide, maximize, close (21 points each, from the right)
mx=$((x0 + fw - 21 - 21 + 10)); by=$((y0 + 14))
poke "click $mx $by"
sleep 0.5
check "ssd: the maximize button maximizes" "flags $SSD_ID | grep -q maximized" "flags $(flags $SSD_ID)"
x0=$(wx $SSD_ID); y0=$(wy $SSD_ID); fw=$(ww $SSD_ID)
poke "click $((x0 + fw - 21 - 21 + 10)) $((y0 + 14))"
sleep 0.5
check "ssd: and again restores it" "! flags $SSD_ID | grep -q maximized" "flags $(flags $SSD_ID)"
x0=$(wx $SSD_ID); y0=$(wy $SSD_ID); fw=$(($(ww $SSD_ID) + 10))
poke "move $((x0 + fw - 11)) $((y0 + 14)); down; sleep 100; move $((x0 + fw / 2)) $((y0 + 150)); sleep 100; up"
sleep 0.5
check "ssd: close pressed, then let go elsewhere: still open" "[ -n \"\$(win $SSD_ID)\" ]" "gone"

# --- the window menu: right click on the title, Roll Window Up
x0=$(wx $SSD_ID); y0=$(wy $SSD_ID)
poke "click $((x0 + 200)) $((y0 + 14)) 2"
sleep 0.3
# rows of 26 after 4 of padding: phone, minimize, maximize, roll up
poke "click $((x0 + 260)) $((y0 + 14 + 4 + 3 * 26 + 13))"
sleep 0.4
check "ssd: the window menu rolls it up" "flags $SSD_ID | grep -q shaded" "flags $(flags $SSD_ID)"
pix=$(pixel $((x0 + 300)) $((y0 + 29 + 40)))
check "ssd: rolled up, the app is not shown under the title" "[ $pix != 000000 ] && [ $pix != 1a1a1a ]" "pixel $pix"
c desktop pointer $((x0 + 200)) $((y0 + 14))
poke "click $((x0 + 200)) $((y0 + 14)) 2"
sleep 0.3
poke "click $((x0 + 260)) $((y0 + 14 + 4 + 3 * 26 + 13))"
sleep 0.4
check "ssd: and down again" "! flags $SSD_ID | grep -q shaded" "flags $(flags $SSD_ID)"

# --- a request with no button down is not a grab (a late GTK move)
x0=$(wx $SSD_ID)
c desktop pointer 800 400
poke "move 900 500"
check "no stray grab: the window does not follow a bare pointer" "[ $(wx $SSD_ID) = $x0 ]" "moved to $(wx $SSD_ID)"

# --- the theme from xfconf, live
printf 'theme=Kokodi\nbutton_layout=O|HMC\n' > "$XDG_RUNTIME_DIR/ztmetro-xfwm4"
c desktop theme
sleep 0.5
check "xfconf's theme is taken live (Kokodi)" "grep -q \"theme 'Kokodi'\" $LOG" "$(grep 'frames from' $LOG | tail -1)"
shot theme-kokodi
# and as XFCE's settings dialog sets it (xfconf; the session tells the shell)
xfconf-query -c xfwm4 -p /general/theme -n -t string -s Default-hdpi 2>/dev/null ||
	xfconf-query -c xfwm4 -p /general/theme -s Default-hdpi
sleep 1.5
check "xfconf-query's theme reaches the frames (Default-hdpi)" "grep -q \"theme 'Default-hdpi'\" $LOG" \
	"$(grep 'frames from' $LOG | tail -1)"
shot theme-hdpi
if [ -n "$theme0" ]; then
	xfconf-query -c xfwm4 -p /general/theme -s "$theme0"
else
	xfconf-query -c xfwm4 -p /general/theme -r
fi
sleep 1
kill $csd_pid $ssd_pid 2>/dev/null

echo "windows: $pass passed, $fail failed"
