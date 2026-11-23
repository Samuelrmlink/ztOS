#!/bin/sh
# A recents card thrown away (ROADMAP 8.74), past the shell: ztos-android
# close and ztos-waydroid-recover close, with lxc, waydroid and sudo as stubs
# that log what they are asked. Checks that a frozen Android is thawed before
# the force-stop, that only a package name is taken, and that Android's
# freeze policy is applied again after.
# usage: card-dismiss-helper.sh RECOVER_SH ZTOS_ANDROID
set -eu
recover=$1
android=$2
dir=$(mktemp -d /tmp/card-dismiss-helper.XXXXXX)
trap 'rm -rf "$dir"' EXIT
mkdir -p "$dir/bin" "$dir/var" "$dir/home/.config/ztmetro"
log=$dir/log
state=$dir/state
: > "$log"

# the helper, runnable unprivileged: no root check, its paths in $dir
sed -e 's|^\[\[ \$EUID == 0 && |[[ |' \
    -e "s|^\. /usr/lib/ztos/user.sh|ZTOS_HOME=$dir/home; ZTOS_RUNTIME_DIR=$dir|" \
    -e "s|/var/lib/waydroid/waydroid.cfg|$dir/var/waydroid.cfg|" \
    -e "s|/run/ztos-waydroid-settings|$dir/settings|" \
    "$recover" > "$dir/recover"
grep -q "ZTOS_HOME=$dir/home" "$dir/recover" || { echo "card-dismiss-helper: could not stub user.sh"; exit 1; }
chmod +x "$dir/recover"
: > "$dir/var/waydroid.cfg"

cat > "$dir/bin/lxc-info" <<EOF
#!/bin/sh
case "\$*" in
    *-sH*) cat "$state" ;;
    *-pH*) echo 4242 ;;
esac
EOF
cat > "$dir/bin/lxc-attach" <<EOF
#!/bin/sh
# the script and its arguments after "-c"
while [ \$# -gt 0 ] && [ "\$1" != -c ]; do shift; done
shift
case "\$1" in
    *force-stop*) shift; echo "force-stop \$*" >> "$log" ;;
    *) echo "attach-other" >> "$log" ;;
esac
EOF
cat > "$dir/bin/waydroid" <<EOF
#!/bin/sh
echo "waydroid \$*" >> "$log"
[ "\$*" = "container unfreeze" ] && echo RUNNING > "$state"
exit 0
EOF
cat > "$dir/bin/lxc-unfreeze" <<EOF
#!/bin/sh
echo "lxc-unfreeze" >> "$log"
echo RUNNING > "$state"
EOF
cat > "$dir/bin/sudo" <<EOF
#!/bin/sh
echo "sudo \$*" >> "$log"
exit \${SUDO_RC:-0}
EOF
cat > "$dir/bin/ztos-waydroid-power" <<EOF
#!/bin/sh
echo "power \$*" >> "$log"
EOF
chmod +x "$dir/bin/"*
PATH=$dir/bin:$PATH
export PATH

fail() { echo "card-dismiss-helper: $*"; echo "--- log:"; cat "$log"; exit 1; }

# running: force-stopped at once, with the package as an argument (never
# pasted into the script)
echo RUNNING > "$state"; : > "$log"
"$dir/recover" close com.spotify.music || fail "running: exit $?"
[ "$(grep force-stop "$log")" = "force-stop sh com.spotify.music" ] || fail "running: no force-stop"
grep -q unfreeze "$log" && fail "running: thawed for nothing"

# frozen (Spotify a background app on Start): thawed first, then stopped
echo FROZEN > "$state"; : > "$log"
"$dir/recover" close com.spotify.music || fail "frozen: exit $?"
first=$(grep -n "container unfreeze" "$log" | cut -d: -f1)
stop=$(grep -n force-stop "$log" | cut -d: -f1)
[ -n "$first" ] && [ -n "$stop" ] && [ "$first" -lt "$stop" ] || fail "frozen: not thawed before the force-stop"

# half way (FREEZING): thawed as well
echo FREEZING > "$state"; : > "$log"
"$dir/recover" close com.spotify.music || fail "freezing: exit $?"
grep -q "container unfreeze" "$log" || fail "freezing: not thawed"

# Android not running: 3, nothing attached
echo STOPPED > "$state"; : > "$log"
rc=0; "$dir/recover" close com.spotify.music || rc=$?
[ "$rc" = 3 ] || fail "stopped: exit $rc, want 3"
grep -q force-stop "$log" && fail "stopped: attached"

# anything but one package name: 2, nothing run
echo RUNNING > "$state"
for bad in 'com.x;reboot' '-rf' 'a b' 'com' '.com.x' 'com.x.' 'com..x' '$(id)' 'com/x' ''; do
    : > "$log"
    rc=0; "$dir/recover" close "$bad" || rc=$?
    [ "$rc" = 2 ] || fail "bad '$bad': exit $rc, want 2"
    [ -s "$log" ] && fail "bad '$bad': ran something"
done
: > "$log"
rc=0; "$dir/recover" close com.a com.b || rc=$?
[ "$rc" = 2 ] || fail "two packages: exit $rc"
rc=0; "$dir/recover" close || rc=$?
[ "$rc" = 2 ] || fail "no package: exit $rc"
[ -s "$log" ] && fail "bad arguments ran something"

# ztos-android close: any Waydroid package (installed by ztOS or not), the
# freeze policy applied again after, no "Unable to open" toast
export XDG_RUNTIME_DIR="$dir"
: > "$log"
python3 "$android" close com.example.notinregistry || fail "ztos-android close: exit $?"
[ "$(sed -n 1p "$log")" = "sudo -n /usr/local/libexec/ztos-waydroid-recover close com.example.notinregistry" ] ||
    fail "ztos-android close: helper not run"
[ "$(sed -n 2p "$log")" = "power sync" ] || fail "ztos-android close: freeze policy not applied after"
: > "$log"
rc=0; python3 "$android" close 'com.x;reboot' 2>/dev/null || rc=$?
[ "$rc" = 2 ] || fail "ztos-android bad package: exit $rc"
[ -s "$log" ] && fail "ztos-android bad package: ran something"
# Android down (3): not an error
: > "$log"
SUDO_RC=3 python3 "$android" close com.spotify.music || fail "ztos-android: Android down is not an error"
grep -q "power sync" "$log" || fail "ztos-android: no policy after a failed close"

echo "card-dismiss-helper: ok"
