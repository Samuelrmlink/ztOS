#!/bin/sh
# ztmmsd's offline tools, end to end: a notification in two SMS parts joined
# from disk, an m-send-req built from a file and read back, the access point
# for T-Mobile and Mint. No network, no modem; made-up numbers.
#	sh mms/tests/ztmmsd-cli.sh mms/ztmmsd
set -u
D=${1:?ztmmsd}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }

# a WAP push with an m-notification-ind, split over two SMS-DELIVERs
# type, "T0001", 1.2, From +15550100, Content-Location, size 1000
IND=8c82985430303031008d92890b802b31353535303130300083687474703a2f2f6d6d732e6578616d706c652e696e76616c69642f78008e0203e8
PUSH=010603beaf84$IND
LEN=$(( ${#PUSH} / 2 ))
HALF=$(( LEN / 2 ))
P1=$(echo "$PUSH" | cut -c1-$(( HALF * 2 )))
P2=$(echo "$PUSH" | cut -c$(( HALF * 2 + 1 ))-)
sms() {	# part total data: centre, SMS-DELIVER with a header, from +5555011000,
	# 8-bit, ports 2948/9200, concatenation ref 0x42
	n=$(( ${#3} / 2 ))
	printf '07915155100000F0440A915555100100000462112110000000%02X0B05040B8423F0000342%02X%02X%s\n' \
		$(( n + 12 )) "$2" "$1" "$3"
}
S1=$(sms 1 2 "$P1")
S2=$(sms 2 2 "$P2")
OUT=$("$D" --push "$T" "$S2" 2>&1)
case "$OUT" in *notification*|*"not a WAP"*) bad "a part alone waits ($OUT)";; *) ok "a part alone waits";; esac
OUT=$("$D" --push "$T" "$S1" 2>&1)
case "$OUT" in
*"location set"*"tid set"*"from set"*) ok "two parts make the notification";;
*) bad "two parts make the notification: $OUT";;
esac
[ -z "$(ls "$T" | grep part-)" ] && ok "no parts left behind" || bad "parts left behind"

# a send request with a picture, read back
printf '\377\330\377\340fake-jpeg-bytes' > "$T/pic.jpg"
if "$D" --encode-send "$T/req.pdu" "+15550100,5550101" "hi there" "$T/pic.jpg" &&
   "$D" --decode "$T/req.pdu" > "$T/req.txt"; then
	grep -q "^type 0x80" "$T/req.txt" && grep -q "^to +15550100$" "$T/req.txt" && grep -q "^to 5550101$" "$T/req.txt" &&
	grep -q "image/jpeg .*name=image_0.jpg" "$T/req.txt" && grep -q "text/plain charset=utf-8" "$T/req.txt" &&
		ok "a send request with a picture" || { bad "a send request with a picture"; cat "$T/req.txt"; }
else
	bad "a send request could not be built"
fi
"$D" --encode-send "$T/bad.pdu" "not-a-number" "x" 2>/dev/null && bad "a bad number refused" || ok "a bad number refused"
head -c 1100000 /dev/zero > "$T/big.jpg"
"$D" --encode-send "$T/big.pdu" "+15550100" "x" "$T/big.jpg" 2>/dev/null && bad "too big refused" || ok "too big refused"

# the access points
X=$(dirname "$0")
"$D" --apn 310260 "$X/apns-stock-fixture.xml" "$X/../apns-extra.xml" | grep -q "^mmsc=http://mms.msg.eng.t-mobile.com/mms/wapenc$" &&
	ok "T-Mobile's MMSC" || bad "T-Mobile's MMSC"
"$D" --apn 310240 756DFFFF "$X/apns-stock-fixture.xml" "$X/../apns-extra.xml" | grep -q "^apn=Wholesale$" &&
	ok "Mint's access point" || bad "Mint's access point"
exit $fail
