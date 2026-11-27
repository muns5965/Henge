#!/bin/bash
# g7-jit2: host test of core/jitstats.h (the "translated before" bitmaps and exact set behind the retranslation figures). Any Linux, any CPU.
# Plain and ASan + UBSan. The JIT itself cannot run here (arm64 only); tools/check-jit-drop-sites.sh is the static half.
# Usage: [CC=cc] tools/jitstats-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}
mkdir -p "$OUT"
W="-std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -g"
rc=0
for mode in plain asan; do
  case $mode in plain) F="-O2";; asan) F="-O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined";; esac
  echo "=== jitstats ($mode)"
  $CC $W $F tests/test_jitstats.c -lm -o "$OUT/test_jitstats_$mode" 2> "$OUT/jitstats_$mode.log" || { echo "FAILED  build ($mode)"; head -40 "$OUT/jitstats_$mode.log"; rc=1; continue; }
  timeout 300 "$OUT/test_jitstats_$mode" > "$OUT/jitstats_$mode.out" 2>&1 || rc=1
  cat "$OUT/jitstats_$mode.out"
done
exit $rc
