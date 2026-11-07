#!/bin/bash
# push the ztmetro sources and the build script, start the build on the phone, install Notes
cd "$(dirname "$0")/../.."
ROOT=$PWD
. scripts/phone-lib.sh
tar cf build/ztm.tar -C ztmetro --exclude=ref .
timeout 60 socat -u OPEN:build/ztm.tar TCP-LISTEN:5581,bind=172.16.42.2,reuseaddr > /dev/null 2>&1 &
sleep 1
timeout 60 "$PHONE_DO" "mkdir -p /tmp/pending && cat < /dev/tcp/172.16.42.2/5581 > /tmp/pending/ztm.tar; ls -la /tmp/pending/ztm.tar" < /dev/null
audio/push.sh build/allbuild.sh
timeout 60 "$PHONE_DO" --root "date -s @$(date +%s) > /dev/null; setsid -f sh /tmp/zrn/allbuild.sh > /tmp/zrn/allbuild.log 2>&1 < /dev/null; echo build started" < /dev/null
cd ../SDKTEST/notes && ZTOS_PASSWORD="$ZT_PW" timeout 150 ztos-sdk install
echo DEP-DONE
