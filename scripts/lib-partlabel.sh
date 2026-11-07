# Sourced by the flash scripts: check a partition's GPT label on the phone
# before writing to it. The kernel's own record, so it works the same from the
# rescue shell, from TWRP and from the running system, and needs no root.
#
#   require_part_label LABEL /dev/sdXN RUNNER [RUNNER ARGS...]
#
# RUNNER runs one shell command on the phone, passed as its last argument, and
# prints the output. Anything else in that output (a shell's echo, su's prompt)
# is ignored. Returns non-zero, and says why, unless the label matches.
require_part_label() {
	local want=$1 node=$2 got
	shift 2
	got=$("$@" "cat /sys/class/block/${node##*/}/uevent" 2>/dev/null |
		tr -d '\r' | sed -n 's/^PARTNAME=//p' | tail -n 1)
	if [ "$got" != "$want" ]; then
		echo "refusing to write: $node is labelled '${got:-unknown}', expected $want" >&2
		return 1
	fi
}
