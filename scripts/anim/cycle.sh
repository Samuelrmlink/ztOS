#!/bin/bash
# cycle.sh NAME [on|off]: build ztmetro on the phone, install, restart, record a launch and return, fetch
cd "$(dirname "$0")/../.."
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$PWD
. scripts/phone-lib.sh
timeout 400 scripts/ztmetro-push.sh 2>&1 | grep -E " error" | head -3
timeout 300 "$PHONE_DO" 'cd ~/ztmetro && touch ztmetro.c core/*.c && make 2>&1 | grep -E "error|(ztmetro|zrnmetrocore).c.*warning" | head -3' 2>&1 | tail -3
timeout 120 "$PHONE_DO" --root 'make -s -C "$ZT_HOME/ztmetro" install 2>&1 | grep -v -i skew | tail -1; /usr/local/sbin/zboot-session ztmetro | tail -1' 2>&1 | tail -1
timeout 60 bash -c 'until timeout 10 "$PHONE_DO" "test -p \$XDG_RUNTIME_DIR/ztmetro.ctl && pgrep -x ztmetro >/dev/null && echo up" 2>/dev/null | grep -q up; do sleep 2; done'
sleep 4
audio/push.sh $S/rec.sh >/dev/null 2>&1
timeout 90 "$PHONE_DO" "sh /tmp/zrn/rec.sh ${2:-on} 9" 2>&1 | tail -4
$S/fetch.sh $1
