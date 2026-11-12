#!/bin/sh
# Read-only snapshot of every regulator the ztOS kernel knows about.
#   ssh ... samuel@ztos 'sh -s' < power/rails/rail-snapshot.sh
# Prints one line per regulator: index|name|state|microvolts|num_users|
# suspend_mem_state|consumer links. Reads /sys only; no root, no writes.
uname -rv; uptime; date
cat /sys/class/backlight/*/brightness 2>/dev/null | head -1 | sed 's/^/backlight=/'
for r in /sys/class/regulator/regulator.*; do
  printf "%s|%s|%s|%s|%s|%s|" "${r##*.}" "$(cat $r/name)" \
    "$(cat $r/state 2>/dev/null)" "$(cat $r/microvolts 2>/dev/null)" \
    "$(cat $r/num_users)" "$(cat $r/suspend_mem_state 2>/dev/null)"
  for c in "$r"/*; do
    [ -L "$c" ] || continue
    case "${c##*/}" in device|subsystem|supply) ;; *) printf "%s " "${c##*/}" ;; esac
  done
  echo
done
