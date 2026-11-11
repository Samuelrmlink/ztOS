#!/bin/sh
# ztdesk's view: a pointer move to the frame it is shown in, timed with
# zt-desklat for ztdesk's old way of asking for frames and the new one.
#
#   ztmetro/desktop/dev/run.sh --test latency
set -u
pass=0 fail=0
ok() { pass=$((pass + 1)); echo "PASS $*"; }
no() { fail=$((fail + 1)); echo "FAIL $*"; }
for i in 1 2; do
	old=$(./desktop/zt-desklat HEADLESS-2 1600 900 old 300)
	new=$(./desktop/zt-desklat HEADLESS-2 1600 900 new 300)
	echo "$old"
	echo "$new"
done
m_old=$(echo "$old" | sed -n 's/.*median \([0-9.]*\) ms.*/\1/p')
m_new=$(echo "$new" | sed -n 's/.*median \([0-9.]*\) ms.*/\1/p')
f_old=$(echo "$old" | sed -n 's/.* \([0-9.]*\) frames\/s.*/\1/p')
f_new=$(echo "$new" | sed -n 's/.* \([0-9.]*\) frames\/s.*/\1/p')
if [ "$(awk -v a="$m_new" -v b="$m_old" 'BEGIN { print (a < b) }')" = 1 ]; then
	ok "the new way is quicker: median $m_old -> $m_new ms"
else
	no "the new way is not quicker: median $m_old -> $m_new ms"
fi
if [ "$(awk -v a="$f_new" -v b="$f_old" 'BEGIN { print (a > b) }')" = 1 ]; then
	ok "and shows more frames: $f_old -> $f_new a second"
else
	no "not more frames: $f_old -> $f_new a second"
fi
echo "latency: $pass passed, $fail failed"
