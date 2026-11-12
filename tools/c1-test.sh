#!/bin/bash
# C1-1: tests/test_c1shm.c over host/frame_shm.c, and host/ui_runner.c as a real separate process. Linux, no VM, no Android, no network.
# Plain, AddressSanitizer + UndefinedBehaviorSanitizer, and ThreadSanitizer (the runner is built the same way as the test). Then tools/check-runner-safe.sh on the plain runner.
# Usage: [CC=cc] tools/c1-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
rc=0
for mode in ${MODES:-plain asan tsan}; do
  case $mode in
    plain) F="-O1";;
    asan)  F="-O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined";;
    tsan)  F="-O1 -fsanitize=thread";;
  esac
  echo "=== c1 $mode"
  if ! $CC -std=gnu11 -Wall -Wextra -Werror -g $F host/ui_runner.c host/frame_shm.c host/display_buf.c host/key_map.c -o "$OUT/ui_runner_$mode" -lm -lpthread 2> "$OUT/runner_$mode.build.log"; then
    echo "FAILED build of the runner ($mode)"; head -20 "$OUT/runner_$mode.build.log"; rc=1; continue
  fi
  if ! $CC -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-result -g $F tests/test_c1shm.c host/frame_shm.c host/display_buf.c host/touch_input.c -o "$OUT/c1_$mode" -lm -lpthread 2> "$OUT/c1_$mode.build.log"; then
    echo "FAILED build of the test ($mode)"; head -20 "$OUT/c1_$mode.build.log"; rc=1; continue
  fi
  if ! timeout 300 "$OUT/c1_$mode" "$OUT/ui_runner_$mode" > "$OUT/c1_$mode.out" 2> "$OUT/c1_$mode.err"; then
    echo "FAILED run ($mode)"; grep -E "^(FAIL|c1shm:)" "$OUT/c1_$mode.out" | head -30; head -30 "$OUT/c1_$mode.err"; rc=1; continue
  fi
  grep -E "^   \(" "$OUT/c1_$mode.out"; tail -1 "$OUT/c1_$mode.out"
  [ -s "$OUT/c1_$mode.err" ] && { echo "unexpected stderr output ($mode)"; head -10 "$OUT/c1_$mode.err"; rc=1; }
done
tools/check-runner-safe.sh "$OUT/ui_runner_plain" || rc=1
[ $rc = 0 ] && echo "C1-1 OK" || echo "C1-1 FAILED"
exit $rc
