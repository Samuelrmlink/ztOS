#!/bin/bash
cd "$(dirname "$0")/../.." || exit 1
ROOT=$PWD
. scripts/phone-lib.sh
audio/push.sh build/rec3.sh
timeout 60 "$PHONE_DO" "pgrep -a ztmetro | head -1; sh /tmp/zrn/rec3.sh" < /dev/null
timeout 30 "$PHONE_DO" "rm -rf /tmp/ztrec-kb; mv /tmp/ztrec /tmp/ztrec-kb; mv /tmp/ztrec-open /tmp/ztrec" < /dev/null
timeout 100 scripts/anim/fetch.sh notes-open3
timeout 30 "$PHONE_DO" "rm -rf /tmp/ztrec; mv /tmp/ztrec-kb /tmp/ztrec" < /dev/null
timeout 100 scripts/anim/fetch.sh notes-kb
echo RUN-DONE
