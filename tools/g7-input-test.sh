#!/bin/bash
# Guest input injector and touch transform tests, plain and ASan+UBSan, on any Linux. No VM is started. Usage: [CC=cc] tools/g7-input-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}; mkdir -p "$OUT"; rc=0
for mode in plain asan; do
  case $mode in plain) F="-O1";; asan) F="-O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined";; esac
  for t in "test_input_inject host/input_inject.c host/touch_input.c" "test_touch_transform host/touch_input.c" "test_touch_display host/touch_input.c"; do
    set -- $t; n=$1; shift
    $CC -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -g $F tests/$n.c "$@" -o "$OUT/${n}_$mode" -lm 2> "$OUT/${n}_$mode.log" || { echo "FAILED build $n ($mode)"; head -20 "$OUT/${n}_$mode.log"; rc=1; continue; }
    "$OUT/${n}_$mode" | sed "s/^/[$mode] /" | grep -E "FAIL|^\[$mode\] PASS $n"; "$OUT/${n}_$mode" > /dev/null || rc=1
  done
done
[ $rc = 0 ] && echo "G7 INPUT TEST OK" || echo "G7 INPUT TEST FAILED"
exit $rc
