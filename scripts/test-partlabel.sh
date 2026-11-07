#!/bin/bash
# require_part_label against a stand-in for the phone. Needs no phone.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib-partlabel.sh"
fail=0

# prints what the phone would, with some leading noise and telnet's line ends
fake() {
	printf 'Password: \r\n%s\r\n' "$FAKE_OUT"
}

check() {	# name, wanted status, label, phone's output
	local st
	FAKE_OUT=$4 require_part_label "$3" /dev/sda10 fake 2>/dev/null
	st=$?
	if [ "$st" = "$2" ]; then echo "ok   $1"; else echo "FAIL $1: status $st"; fail=1; fi
}

check match      0 BOOT $'MAJOR=8\r\nDEVNAME=sda10\r\nPARTNAME=BOOT'
check wrong      1 BOOT $'MAJOR=8\r\nDEVNAME=sda10\r\nPARTNAME=EFS'
check prefix     1 BOOT $'PARTNAME=BOOTA'
check no-label   1 BOOT $'MAJOR=8\r\nDEVNAME=sda10'
check no-answer  1 BOOT ''
exit $fail
