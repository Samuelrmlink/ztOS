#!/bin/bash
# Run one command on the phone, as its user or as root (see scripts/phone-lib.sh).
#
#   scripts/phone-do.sh [--root] 'command'
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$ROOT/scripts/phone-lib.sh"

root=
if [ "${1:-}" = --root ]; then
	root=1
	shift
fi
cmd=${1:?a command}

if [ -n "$root" ]; then
	# one word for the shell on the phone
	cmd="env ZT_HOME=\"\$HOME\" sh -c '${cmd//\'/\'\\\'\'}'"
fi

if [ -n "$ZT_SSH" ]; then
	read -ra args <<< "$ZT_SSH"
	exec ssh -o BatchMode=yes -o ConnectTimeout=10 "${args[@]/#\~/$HOME}" "${root:+sudo -n }$cmd"
fi
# the login's password, then sudo's, which is the same one
exec "$ROOT/scripts/ssh-pw.py" "$ZT_USER" "$ZT_PW${root:+,$ZT_PW}" "$PHONE" "${root:+sudo -S -p 'Password: ' }$cmd"
