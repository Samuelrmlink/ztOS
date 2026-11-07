# Who and where the phone is, for scripts on this computer. Source it from a bash
# script, once $ROOT is the repository, then run commands with scripts/phone-do.sh,
# which `timeout` can wrap:
#
#   . "$ROOT/scripts/phone-lib.sh"
#   timeout 30 "$PHONE_DO" 'uname -r'             as the phone's user
#   timeout 30 "$PHONE_DO" --root 'dmesg | tail'  as root, through sudo
#
# Works whatever the phone's user is called. Set in the environment:
#
#   ZT_USER   the user on the phone. Default: your name on this computer. A phone
#             installed with the defaults is ztos; a release image is zerone; the
#             older hand-made image is alarm.
#   ZT_PW     that user's password. Asked for once if there's a terminal.
#   ZT_SSH    ssh arguments, if you log in with a key, as for scripts/ztctl.py:
#             ZT_SSH='-i ~/.ssh/ztos_phone user@172.16.42.1'. ZT_USER and ZT_PW are
#             then not used, and sudo must not ask for a password.
#   PHONE     its address (172.16.42.1), HOST this computer's (172.16.42.2)
#
# Root goes through sudo, not su: an installer build locks root's password, and
# its user is in wheel. In the root commands, $ZT_HOME is the user's home.
PHONE=${PHONE:-172.16.42.1}
HOST=${HOST:-172.16.42.2}
ZT_USER=${ZT_USER:-$(id -un)}
ZT_SSH=${ZT_SSH:-}
ZT_PW=${ZT_PW:-}
PHONE_DO=$ROOT/scripts/phone-do.sh

if [ -z "$ZT_SSH" ] && [ -z "$ZT_PW" ]; then
	if [ -t 0 ]; then
		read -rsp "password for $ZT_USER on the phone: " ZT_PW
		echo >&2
	else
		echo "set ZT_PW to the password of $ZT_USER on the phone (or ZT_USER to name another user, or ZT_SSH to use a key)" >&2
		exit 1
	fi
fi
export PHONE HOST ZT_USER ZT_SSH ZT_PW PHONE_DO
