#!/bin/sh
# Apps told where their windows are (zt_environment_v1), and our own apps
# adapting while they run: run inside desktop/dev/run.sh
#
#   ztmetro/desktop/dev/run.sh --test env
#
# The AirPods app (ztpods, ZrnMetroCore) opens on the phone and is moved to
# the desktop and back with the control pipe's `desktop move` and `desktop
# phone`, as the switcher and the title bar do. It says what it is told
# (ZMC_TRACE_ENV) and the shell keeps $XDG_RUNTIME_DIR/ztmetro-env/PID.
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
waitfor() { # waitfor SECONDS 'condition'
	i=0
	while [ $i -lt $(($1 * 10)) ]; do eval "$2" && return 0; sleep 0.1; i=$((i + 1)); done
	return 1
}
envfile() { cat "$XDG_RUNTIME_DIR/ztmetro-env/$pid" 2>/dev/null; }

ZMC_TRACE_ENV=1 ./pods/ztpods > /tmp/pods.log 2>&1 &
pid=$!
waitfor 8 'grep -q "environment" /tmp/pods.log'
check "ztpods is told it is on the phone" "grep -q 'environment phone' /tmp/pods.log" "$(cat /tmp/pods.log)"
check "the env file says phone" "envfile | grep -q '^ztpods phone HEADLESS-1'" "$(envfile)"
check "not on the desktop" "[ -z \"\$(win ztpods)\" ]" "$(win ztpods)"

waitfor 8 'grep -q ztpods $XDG_RUNTIME_DIR/ztmetro-apps 2>/dev/null' || sleep 2
c desktop move ztpods
waitfor 5 'grep -q "environment desktop" /tmp/pods.log'
check "moved: ztpods is told it is on the desktop, and adapts (its own callback ran)" \
	"grep -q 'environment desktop' /tmp/pods.log && grep -q 'ztpods: on the desktop' /tmp/pods.log" "$(tail -3 /tmp/pods.log)"
check "the same process (not restarted)" "kill -0 $pid" "gone"
check "the env file says desktop" "envfile | grep -q '^ztpods desktop HEADLESS-2'" "$(envfile)"
line=$(win ztpods)
check "on the desktop it has the shell's frame (it asked for server-side decorations)" \
	"echo \"\$line\" | grep -q framed" "$line"
x=$(echo "$line" | awk '{print $4 - '$OX'}'); y=$(echo "$line" | awk '{print $5}')
w=$(echo "$line" | awk '{print $6}'); h=$(echo "$line" | awk '{print $7}')
grim -o "$OUT" /out/env-desktop.png

# the mouse works in it
grim -o "$OUT" -t ppm /tmp/a.ppm
# a drag with the mouse turns the page, as a finger's swipe does
poke "drag $((x + 5 + w - 40)) $((y + 29 + h / 2)) $((x + 5 + 40)) $((y + 29 + h / 2)) 300 12"
sleep 1.2
grim -o "$OUT" -t ppm /tmp/b.ppm
check "a mouse drag in it turns the page (the pointer acts as a finger)" "! cmp -s /tmp/a.ppm /tmp/b.ppm" "no change"
grim -o "$OUT" /out/env-desktop-dragged.png

# resizable, within its limits
fw=$((w + 10))
poke "drag $((x + fw - 2)) $((y + 200)) $((x + fw + 1200)) $((y + 200)) 300 10"
sleep 0.6
w2=$(win ztpods | awk '{print $6}')
check "resized wider, it stops at its maximum width (900)" "[ \"$w2\" -gt $w ] && [ \"$w2\" -le 900 ]" "width $w -> $w2"
poke "drag $((x + w2 + 8)) $((y + 200)) $((x + 100)) $((y + 200)) 300 10"
sleep 0.6
w3=$(win ztpods | awk '{print $6}')
check "narrower, it stops at its minimum (340)" "[ \"$w3\" -ge 340 ] && [ \"$w3\" -lt \"$w2\" ]" "width $w2 -> $w3"

c desktop phone ztpods
waitfor 5 'tail -1 /tmp/pods.log | grep -q "environment phone"'
check "back to the phone: told so, still the same process" \
	"tail -2 /tmp/pods.log | grep -q 'environment phone' && kill -0 $pid" "$(tail -2 /tmp/pods.log)"
check "the env file says phone again" "envfile | grep -q '^ztpods phone'" "$(envfile)"
grim -o HEADLESS-1 /out/env-phone.png
kill $pid 2>/dev/null
sleep 0.5
check "the env file goes with the app" "[ ! -e $XDG_RUNTIME_DIR/ztmetro-env/$pid ]" "$(envfile)"

echo "env: $pass passed, $fail failed"
