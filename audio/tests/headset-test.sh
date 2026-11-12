#!/bin/sh
# Host test of the jack's arithmetic (audio/driver/zrn-headset-logic.h).
set -e
cd "$(dirname "$0")"
out=${TMPDIR:-/tmp}/zrn-headset-test
cc -std=c11 -Wall -Wextra -Werror -o "$out" headset-test.c
"$out"
