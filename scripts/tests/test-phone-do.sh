#!/bin/bash
# scripts/phone-do.sh and scripts/ssh-pw.py, with stand-ins for ssh and sudo that
# ask for passwords the way the real ones do. The "phone" is this computer: no
# network, and nothing runs as root.
#
#   scripts/tests/test-phone-do.sh
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
fails=0
check() {	# NAME WANT GOT
	if grep -qF -- "$2" <<< "$3"; then
		echo "ok    $1"
	else
		echo "FAIL  $1: wanted \"$2\", got:"; sed 's/^/      /' <<< "$3"
		fails=$((fails + 1))
	fi
}
mkdir -p "$W/bin" "$W/phone-bin" "$W/home"

# ssh: logs who it was asked to log in as, wants the password, runs the command
cat > "$W/bin/ssh" <<'EOF'
#!/bin/bash
args=("$@"); cmd=${args[-1]}; login=${args[-2]}
echo "login $login" >> "$LOG"
if [[ " ${args[*]} " != *" BatchMode=yes "* ]]; then
	printf "%s's password: " "$login" > /dev/tty
	IFS= read -r pw < /dev/tty
	[ "$pw" = secret ] || { echo "Permission denied" > /dev/tty; exit 255; }
fi
exec < /dev/tty > /dev/tty 2>&1
PATH=$PHONE_BIN:$PATH HOME=$PHONE_HOME exec bash -c "$cmd"
EOF
# sudo: -S takes the password from stdin, -n never asks, and it must be asked for
cat > "$W/phone-bin/sudo" <<'EOF'
#!/bin/bash
echo "sudo $*" >> "$LOG"
mode=$1; shift
case $mode in
-n) [ -n "${SUDO_NOPASSWD:-}" ] || { echo "sudo: a password is required"; exit 1; } ;;
-S)
	prompt=$2; shift 2
	printf '%s' "$prompt"
	IFS= read -r pw
	[ "$pw" = secret ] || { echo "Sorry, try again."; exit 1; }
	;;
*) echo "sudo was run without -S or -n"; exit 2 ;;
esac
export IS_ROOT=yes
exec "$@"
EOF
chmod +x "$W/bin/ssh" "$W/phone-bin/sudo"
export LOG=$W/log PHONE_BIN=$W/phone-bin PHONE_HOME=$W/home PATH=$W/bin:$PATH

DO=$ROOT/scripts/phone-do.sh

out=$(ZT_USER=anyone ZT_PW=secret timeout 20 "$DO" 'echo plain:$HOME' < /dev/null 2>&1)
check "a command as the user" "plain:$W/home" "$out"
check "logged in as ZT_USER" "login anyone@172.16.42.1" "$(cat "$LOG")"

out=$(ZT_USER=anyone ZT_PW=secret timeout 20 "$DO" --root 'echo "root:$IS_ROOT:$ZT_HOME"' < /dev/null 2>&1)
check "root through sudo, with the user's home" "root:yes:$W/home" "$out"

out=$(ZT_USER=anyone ZT_PW=secret timeout 20 "$DO" --root "echo 'it'\\''s quoted'" < /dev/null 2>&1)
check "quotes survive the trip" "it's quoted" "$out"

: > "$LOG"
out=$(ZT_USER=" " ZT_PW= ZT_SSH='-i ~/key me@phone' SUDO_NOPASSWD=1 timeout 20 "$DO" --root 'echo "key:$IS_ROOT"' < /dev/null 2>&1)
check "a key: no password, sudo -n" "key:yes" "$out"
check "a key: ~ is worked out" "login me@phone" "$(cat "$LOG")"

out=$(ZT_USER=anyone ZT_PW= timeout 20 "$DO" 'true' < /dev/null 2>&1)
check "no password and no terminal: says what to set" "set ZT_PW" "$out"

out=$(env -u ZT_USER ZT_PW=secret timeout 20 "$DO" 'true' < /dev/null 2>&1; cat "$LOG")
check "the default user is the one on this computer" "login $(id -un)@172.16.42.1" "$out"

[ "$fails" = 0 ] && echo "all passed" || { echo "$fails failed"; exit 1; }
