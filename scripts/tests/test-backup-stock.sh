#!/bin/bash
# scripts/backup-stock.sh and flash.sh's stock/ check, against a stand-in for adb.
# No phone. The script is copied into a scratch tree, so the real stock/ is safe.
#
#   scripts/tests/test-backup-stock.sh
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
fails=0
ok() { echo "ok    $1"; }
bad() { echo "FAIL  $1"; fails=$((fails + 1)); }

mkdir -p "$W/tree/scripts" "$W/phone"
cp "$ROOT/scripts/backup-stock.sh" "$ROOT/scripts/flash.sh" "$W/tree/scripts/"
for p in BOOT RECOVERY EFS; do head -c 2097152 /dev/urandom > "$W/phone/$p"; done

# every command this adb sees is logged; only reads are understood
cat > "$W/adb" <<'EOF'
#!/bin/bash
echo "$*" >> "$PHONE/commands"
case $1 in
get-state) echo recovery; exit 0 ;;
shell|exec-out) shift ;;
*) echo "unexpected adb $1" >&2; exit 9 ;;
esac
cmd=$*
by=/dev/block/platform/11120000.ufs/by-name
cmd=${cmd//$by/$PHONE}
case $cmd in
"getprop ro.product.device") echo starlte ;;
"blockdev --getsize64 "*) stat -c %s "${cmd##* }" ;;
"sha256sum "*) sha256sum "${cmd##* }" ;;
"dd if="*) if [ -n "${CORRUPT:-}" ]; then head -c 2097152 /dev/urandom; else sh -c "$cmd"; fi ;;
"ls "*) sh -c "$cmd" ;;
*) echo "not a read: $cmd" >&2; exit 9 ;;
esac
EOF
chmod +x "$W/adb"
export PHONE=$W/phone ADB=$W/adb
S=$W/tree/stock

out=$(CORRUPT=1 "$W/tree/scripts/backup-stock.sh" 2>&1)
[ $? != 0 ] && [ ! -e "$S/BOOT.img" ] && [ ! -e "$S/BOOT.img.part" ] && ok "a bad copy is not kept" || bad "bad copy: $out"

out=$("$W/tree/scripts/backup-stock.sh" 2>&1)
if [ $? = 0 ] && cmp -s "$S/BOOT.img" "$PHONE/BOOT" && cmp -s "$S/RECOVERY.img" "$PHONE/RECOVERY" && cmp -s "$S/EFS.img" "$PHONE/EFS"; then
	ok "all three saved, identical to the phone's"
else
	bad "backup: $out"
fi
[ "$(stat -c %a "$S/EFS.img")" = 600 ] && ok "EFS.img is private" || bad "EFS.img mode"
[ ! -e "$S/CPEFS.img" ] && ok "no CPEFS on this phone, none asked for" || bad "CPEFS"
grep -qE ' of=|mkfs|mount|rm ' "$PHONE/commands" && bad "a write was sent to the phone" || ok "only reads were sent to the phone"

head -c 100 /dev/urandom > "$PHONE/BOOT"
"$W/tree/scripts/backup-stock.sh" > /dev/null 2>&1
[ "$(stat -c %s "$S/BOOT.img")" = 2097152 ] && ok "an existing backup is never replaced" || bad "the first BOOT.img was replaced"

rm "$S/EFS.img"
out=$("$W/tree/scripts/flash.sh" BOOT /dev/null 2>&1)
if [ $? != 0 ] && grep -q "scripts/backup-stock.sh" <<< "$out"; then
	ok "flash.sh refuses without EFS.img, and says what to run"
else
	bad "flash.sh: $out"
fi

[ "$fails" = 0 ] && echo "all passed" || { echo "$fails failed"; exit 1; }
