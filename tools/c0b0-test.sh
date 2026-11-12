#!/bin/bash
# C0 + B0: tests/test_c0b0.c over host/display_buf.c, host/key_map.c, host/touch_input.c. Linux, no VM, no Android, no network.
# Runs the tests plain, with AddressSanitizer + UndefinedBehaviorSanitizer, and with ThreadSanitizer (the display buffer's lock and close contract).
# Usage: [CC=cc] tools/c0b0-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}
mkdir -p "$OUT"
SRC="tests/test_c0b0.c host/display_buf.c host/key_map.c host/touch_input.c"
rc=0
for mode in plain asan tsan; do
  case $mode in
    plain) F="-O1";;
    asan)  F="-O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined";;
    tsan)  F="-O1 -fsanitize=thread";;
  esac
  echo "=== c0b0 $mode"
  if ! $CC -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -g $F $SRC -o "$OUT/c0b0_$mode" -lm -lpthread 2> "$OUT/c0b0_$mode.build.log"; then
    echo "FAILED build ($mode)"; head -20 "$OUT/c0b0_$mode.build.log"; rc=1; continue
  fi
  if ! timeout 300 "$OUT/c0b0_$mode" > "$OUT/c0b0_$mode.out" 2> "$OUT/c0b0_$mode.err"; then
    echo "FAILED run ($mode)"; grep -E "^(FAIL|c0b0:)" "$OUT/c0b0_$mode.out" | head -30; head -30 "$OUT/c0b0_$mode.err"; rc=1; continue
  fi
  tail -1 "$OUT/c0b0_$mode.out"
  [ -s "$OUT/c0b0_$mode.err" ] && { echo "unexpected stderr output ($mode)"; head -10 "$OUT/c0b0_$mode.err"; rc=1; }
done
[ $rc = 0 ] && echo "C0B0 OK" || echo "C0B0 FAILED"
exit $rc
