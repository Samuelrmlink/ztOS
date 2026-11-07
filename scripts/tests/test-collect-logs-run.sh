#!/bin/bash
# A whole run of scripts/collect-logs.py against a stand-in for ssh: the result is
# a private tar.gz, with no identity in any file. No phone.
#
#   scripts/tests/test-collect-logs-run.sh
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir "$W/bin"
cat > "$W/bin/ssh" <<'EOF'
#!/bin/sh
printf 'IMEI 490154203237518\nok 3c:37:86:aa:bb:cc\nnet CoffeeShop up\n'
EOF
chmod +x "$W/bin/ssh"
PATH=$W/bin:$PATH ZT_SSH=phone "$ROOT/scripts/collect-logs.py" --out "$W/out" || { echo "FAIL  the run"; exit 1; }
tarball=$(ls "$W"/out/*.tar.gz)
[ "$(stat -c %a "$tarball")" = 600 ] || { echo "FAIL  the file is not private"; exit 1; }
[ -z "$(find "$W/out" -name '*.part')" ] || { echo "FAIL  a partial file was left"; exit 1; }
mkdir "$W/x"
tar xzf "$tarball" -C "$W/x"
[ "$(find "$W/x" -type f | wc -l)" = 11 ] || { echo "FAIL  expected 11 files"; exit 1; }
if grep -rqE '490154203237518|3c:37:86|CoffeeShop' "$W/x"; then
	echo "FAIL  identity in the result"; exit 1
fi
grep -rq 'line removed' "$W/x" || { echo "FAIL  nothing was removed"; exit 1; }
echo "all passed"
