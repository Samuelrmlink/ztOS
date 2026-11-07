#!/bin/bash
# Write the Arch root image into USERDATA from the running zboot rescue shell,
# over the USB network, in verified chunks.
#
#   scripts/flash-root.sh [image] [chunk MiB]
#
# Each chunk is compared against the partition first and only sent when it
# differs, then read back and compared again. That makes the whole run
# idempotent and resumable: a transfer killed halfway is finished by running
# the script again, and an unchanged chunk costs one hash on each side.
#
# Two things about busybox on the phone shape this:
#   * `nc -l` never notices the peer closing, so the phone dials out to a
#     listener on the host instead of listening itself;
#   * `dd` counts a short pipe read as a whole record, so `count=` without
#     `iflag=fullblock` stops after 256 32 KB reads (8 MB), not 256 MiB.
# The receiver is started detached and waited for by watching its dd, because
# the phone's shell will not return while nc is still holding the pipeline.
#
# The phone has to reach this computer on port 5557. That is tried first, before
# anything is written, so a firewall gets a clear message and not a silent wait.
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
IMG=${1:-$ROOT/out/arch-root.img}
CHUNK=${2:-256}
PART=/dev/sda25
HOST=${HOST:-172.16.42.2}
PORT=${PORT:-5557}
PSH=${PSH:-$ROOT/scripts/psh.py}
DRAIN_WAIT=${DRAIN_WAIT:-30}	# seconds for the listener to finish once the phone's dd is gone
. "$ROOT/scripts/lib-partlabel.sh"
TMP=$(mktemp -d)
LOG=$TMP/socat.log
server=
trap '[ -z "$server" ] || kill "$server" 2>/dev/null; rm -rf "$TMP"' EXIT

die() {
	echo "flash-root: $*" >&2
	exit 1
}

blocked() {
	cat >&2 <<-EOF
	flash-root: the phone could not connect to this computer on $HOST port $PORT.
	  Most likely a firewall is blocking it. Let the USB interface in, and not Wi-Fi:
	  see "The firewall" in installer/README.md. With firewalld, for example:
	    sudo firewall-cmd --get-active-zones
	    sudo firewall-cmd --zone=<the USB interface's zone> --add-port=$PORT/tcp
	  Nothing is lost: run this script again and it carries on where it stopped.
	EOF
	exit 2
}

# Serve one file to the first phone that connects.
listen() {
	: > "$LOG"
	socat -d -d -u "OPEN:$1" "TCP-LISTEN:$PORT,bind=$HOST,reuseaddr" 2> "$LOG" &
	server=$!
	sleep 1
	if ! kill -0 "$server" 2> /dev/null; then
		wait "$server" 2> /dev/null
		server=
		echo "flash-root: could not listen on $HOST port $PORT:" >&2
		tail -n 3 "$LOG" >&2
		die "does this computer have the address $HOST on the USB interface? See docs/host-setup-rhel.md."
	fi
}

connected() {
	grep -q 'accepting connection' "$LOG"
}

stop_listening() {
	[ -z "$server" ] && return
	kill "$server" 2> /dev/null
	wait "$server" 2> /dev/null
	server=
}

# Can the phone reach the listener? Nothing here writes to the phone's storage.
check_link() {
	local mark=ZTOS_LINK_$$
	echo "$mark" > "$TMP/probe"
	listen "$TMP/probe"
	local got
	got=$("$PSH" -t 15 "nc $HOST $PORT" 2> /dev/null)
	stop_listening
	"$PSH" -t 10 'killall nc 2>/dev/null; true' > /dev/null 2>&1
	case $got in
	*"$mark"*) ;;
	*) blocked ;;
	esac
}

command -v socat > /dev/null || die "socat is not installed on this computer"
[ -s "$IMG" ] || { echo "no image: $IMG" >&2; exit 1; }
SIZE=$(stat -c %s "$IMG")
(( SIZE % (CHUNK * 1048576) == 0 )) || { echo "image size is not a multiple of ${CHUNK}M" >&2; exit 1; }
N=$(( SIZE / (CHUNK * 1048576) ))
echo "image $IMG: $SIZE bytes, $N chunks of ${CHUNK}M -> $PART"

require_part_label USERDATA "$PART" "$PSH" -t 20 || exit 1
psize=$("$PSH" -t 20 "blockdev --getsize64 $PART" | tr -dc 0-9)
[ -n "$psize" ] && [ "$psize" -ge "$SIZE" ] ||
	{ echo "partition too small or unreachable ($psize)" >&2; exit 1; }
"$PSH" -t 10 "touch /run/zboot.hold" >/dev/null 2>&1 || true
[ -n "${SKIP_LINK_CHECK:-}" ] || check_link	# if the check itself is ever wrong, the transfer still has its own limit

# sha256 of one chunk of the partition, as the phone reads it
part_hash() {
	"$PSH" -t 300 "dd if=$PART bs=1M skip=$1 count=$CHUNK iflag=fullblock 2>/dev/null | sha256sum" |
		awk 'NF {print $1; exit}'
}

writer_gone() {
	[ -z "$("$PSH" -t 30 'ps | grep "dd of=\|dd of " | grep -v grep')" ]
}

cleanup_phone() {
	"$PSH" -t 30 'killall nc gunzip dd 2>/dev/null; sync; true' >/dev/null 2>&1
}

sent=0 skipped=0
for ((i = 0; i < N; i++)); do
	off=$(( i * CHUNK ))
	want=$(dd if="$IMG" bs=1M skip="$off" count="$CHUNK" status=none | sha256sum | cut -d' ' -f1)
	have=$(part_hash "$off")
	if [ "$want" = "$have" ]; then
		skipped=$((skipped + 1))
		printf '%3d/%d  skip (identical)  %s\n' "$((i + 1))" "$N" "$(date +%T)"
		continue
	fi
	dd if="$IMG" bs=1M skip="$off" count="$CHUNK" status=none | gzip -1 -c > "$TMP/chunk.gz"
	back=
	for try in 1 2 3; do
		cleanup_phone
		listen "$TMP/chunk.gz"
		"$PSH" -t 60 "setsid sh -c 'nc $HOST $PORT | gunzip | dd of=$PART bs=1M seek=$off count=$CHUNK iflag=fullblock conv=notrunc,fsync' >/dev/null 2>&1 & echo started" >/dev/null
		for ((w = 0; w < 60; w++)); do
			sleep 5
			writer_gone && break
		done
		# the listener ends once the phone has read everything; it must not be
		# waited for without a limit, since nobody may ever have connected
		for ((w = 0; w < DRAIN_WAIT; w++)); do
			kill -0 "$server" 2> /dev/null || break
			sleep 1
		done
		if kill -0 "$server" 2> /dev/null; then
			connected || { stop_listening; cleanup_phone; blocked; }
			echo "   chunk $i: the transfer stalled (try $try)" >&2
		fi
		stop_listening
		cleanup_phone
		back=$(part_hash "$off")
		[ -n "$back" ] || die "the phone stopped answering at chunk $i. Check the cable, then run this again: it carries on."
		[ "$back" = "$want" ] && break
		echo "   chunk $i readback mismatch (try $try)" >&2
	done
	[ "$back" = "$want" ] || { echo "chunk $i failed three times, stopping" >&2; exit 1; }
	sent=$((sent + 1))
	printf '%3d/%d  written and verified  %s\n' "$((i + 1))" "$N" "$(date +%T)"
done
echo "FLASH_ROOT_OK $sent chunk(s) written, $skipped already identical"
