#!/bin/sh
# build the shell with the SDK window matching; the running one is kept as ztmetro.prev
. /usr/lib/ztos/user.sh
cd "$ZTOS_HOME/ztmetro" || exit 1
cp /tmp/zrn/ztmetro.c ztmetro.c && chown "$ZTOS_USER:" ztmetro.c && touch ztmetro.c
su "$ZTOS_USER" -c "make -s ztmetro" 2>&1 | grep -E "error|Error" -A3
if [ -x ztmetro ] && [ ztmetro -nt /usr/local/bin/ztmetro ]; then
	cp -a /usr/local/bin/ztmetro /usr/local/bin/ztmetro.prev
	install -m755 ztmetro /usr/local/bin/ztmetro
	echo INSTALLED
fi
sync
echo BUILD-END
