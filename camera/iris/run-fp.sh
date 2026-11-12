#!/bin/sh
# The fingerprint experiment, on the phone, attended. Approved 2026-11-12.
#	ztlock run iris 'sudo sh ~/iris-probe/run-fp.sh'
# Refuses unless: inside `ztlock run iris`, after 07:00, battery >= 15 %, the
# camera module not loaded, and no unfinished run left a mark. The mark stays
# if the phone resets mid-run; remove it by hand after reading what happened.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
D=/var/lib/zrn-iris
. "$HERE/run-checks.sh"
checks fp || exit 3
mark "fp-irq-probe started $(date +%T)"
{ "$HERE/fp-irq-probe" --watch 30 2>&1; echo $? > "$D/fp.rc"; } | tee "$D/fp-$(date +%Y%m%d-%H%M%S).log"
rc=$(cat "$D/fp.rc" 2>/dev/null || echo 1)
unmark
echo "fp-irq-probe exit $rc"
exit $rc
