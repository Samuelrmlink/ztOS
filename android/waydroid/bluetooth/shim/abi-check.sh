#!/bin/bash
# abi-check.sh OBJDUMP STOCK_SO OBJ...: the libchrome callback code built into
# the shim must work on the stock library's objects: same build mode (NDEBUG,
# no DCHECK members), same layout. For each function below, compares the
# field offsets it reads and writes (memory operands off object pointers; the
# stack and the shadow call stack aside) in the stock library and in the
# shim's objects. Instruction choice may differ: the stock build tail-calls
# where NDK clang doesn't.
set -euo pipefail
objdump=$1 stock=$2
shift 2
syms=(
	_ZN4base8internal13BindStateBaseC2EPFvvEPFvPKS1_E
	_ZN4base8internal12CallbackBaseD2Ev
	_ZN4base8internal27BindStateBaseRefCountTraits8DestructEPKNS0_13BindStateBaseE
	_ZN4base8internal20CallbackBaseCopyableC2ERKS1_
	_ZN4base8internal12CallbackBaseC2ERKNS0_20CallbackBaseCopyableE
)
offsets() {   # the distinct [reg, #offset] operands of one function
	"$objdump" -d --no-show-raw-insn --disassemble-symbols="$1" "${@:2}" 2>/dev/null |
		awk '/>:$/ {on=1; next}
			on && match($0, /\[x([0-9]|1[0-7]|19|2[0-9])(, #-?[0-9]+)?\]/) {
				s = substr($0, RSTART, RLENGTH); sub(/\[x[0-9]+/, "[", s); print s
			}
			on && NF == 0 {on=0}' | sort -u
}
fail=0
for s in "${syms[@]}"; do
	a=$(offsets "$s" "$stock")
	b=$(offsets "$s" "$@")
	if [[ -z $a || -z $b ]]; then
		echo "abi-check: $s missing or touches no fields (stock ${#a}, shim ${#b} chars)"
		fail=1
	elif [[ $a != "$b" ]]; then
		echo "abi-check: $s uses other field offsets than the stock library"
		diff <(echo "$a") <(echo "$b") | head -10
		fail=1
	fi
done
((fail == 0)) && echo "abi-check: libchrome callback code uses the stock library's field offsets (${#syms[@]} functions)"
exit $fail
