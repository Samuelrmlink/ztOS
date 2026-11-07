#!/bin/bash
# apply-pending.sh: put everything written while the phone was away onto it.
# Run on the laptop with the phone on USB (172.16.42.1): the ztMETRO sources
# are pushed and built there, the new boot-time scripts and units installed,
# the vibration and idle modules copied into place. Nothing is restarted and
# nothing risky is switched on: the shell and daemons pick the new builds up
# at the next reboot, and idle power-down stays off until `zrn-cpuidle try`.
set -e
cd "$(dirname "$0")/.."
ROOT=$PWD
# the user and password come from the environment (scripts/phone-lib.sh), never from this file
. scripts/phone-lib.sh
ping -c1 -W2 "$PHONE" > /dev/null || { echo "the phone is not on USB"; exit 1; }

tar cf build/ztm.tar -C ztmetro --exclude=ref .
tar cf build/overlay.tar -C rootfs/overlay usr/local/sbin/zboot-charge-hold usr/local/sbin/zrn-cpuidle \
	etc/systemd/system/zboot-charge-hold.service etc/systemd/system/zrn-cpuidle.service \
	etc/tmpfiles.d/99-ztos-compat.conf
cp haptics/zrn-haptic.ko power/idle/zrn-cpuidle.ko build/
tar rf build/overlay.tar -C build zrn-haptic.ko zrn-cpuidle.ko
for f in ztm overlay; do
	setsid -f timeout 40 socat -u OPEN:build/$f.tar TCP-LISTEN:5581,bind=$HOST,reuseaddr > /dev/null 2>&1 < /dev/null
	sleep 1
	"$PHONE_DO" "mkdir -p /tmp/pending && cat < /dev/tcp/$HOST/5581 > /tmp/pending/$f.tar" > /dev/null 2>&1 < /dev/null
done
"$PHONE_DO" --root 'date -s @'$(date +%s)' > /dev/null
cd "$ZT_HOME/ztmetro" && tar xf /tmp/pending/ztm.tar && chown -R "$(stat -c %U "$ZT_HOME"):" . && touch *.c */*.c
mkdir -p /tmp/pending/o && tar xf /tmp/pending/overlay.tar -C /tmp/pending/o
install -m755 /tmp/pending/o/usr/local/sbin/* /usr/local/sbin/
install -m644 /tmp/pending/o/etc/systemd/system/*.service /etc/systemd/system/
[ -d /tmp/pending/o/etc/tmpfiles.d ] && install -m644 /tmp/pending/o/etc/tmpfiles.d/* /etc/tmpfiles.d/ && systemd-tmpfiles --create /etc/tmpfiles.d/99-ztos-compat.conf 2>/dev/null || true
install -Dm644 -t /usr/lib/modules/$(uname -r)/extra /tmp/pending/o/zrn-haptic.ko /tmp/pending/o/zrn-cpuidle.ko
depmod -a 2>/dev/null
systemctl daemon-reload
systemctl enable zboot-charge-hold.service zrn-cpuidle.service 2>&1 | tail -2
setsid -f sh -c "(make -s 2>&1 | grep -E \"error|Error\" -A3; make -s install 2>&1 | grep -E \"error|Error\"; sync; echo BUILD-END) > /tmp/pending/build.log 2>&1" > /dev/null 2>&1 < /dev/null' < /dev/null | tail -3
echo "building on the phone: /tmp/pending/build.log ends with BUILD-END when done; then reboot it."
