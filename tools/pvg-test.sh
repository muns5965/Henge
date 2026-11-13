#!/bin/bash
# B1-a: the protocol half of the graphics device (devices/pvg_lite.c, compiled unchanged) driven by tests/test_pvg.c through its register windows and guest rings, with a
# flat-RAM machine and a RECORDING stand-in for the Metal side. Any Linux, any CPU (nothing of the engine is linked). Plain and ASan + UBSan. Says nothing about drawing.
# Usage: [CC=cc] tools/pvg-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}
mkdir -p "$OUT"
INC="-Icore -Ihost -Idevices"
W="-std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-result -g"
rc=0
for mode in plain asan; do
  case $mode in
    plain) F="-O1";;
    # alignment is not checked: the guest's rings are 4-byte aligned by the driver, and arm64 (the only place the engine runs) allows unaligned access anyway
    asan)  F="-O1 -fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined";;
  esac
  echo "=== B1-a ($mode)"
  $CC $W $F $INC devices/pvg_lite.c tests/test_pvg.c -lpthread -o "$OUT/test_pvg_$mode" 2> "$OUT/pvg_$mode.log" || { echo "FAILED  build ($mode)"; head -40 "$OUT/pvg_$mode.log"; rc=1; continue; }
  # the device has no destroy function and the test makes many: leak checking is off (it would only count those)
  ASAN_OPTIONS=detect_leaks=0 timeout 120 "$OUT/test_pvg_$mode" all > "$OUT/pvg_$mode.out" 2> "$OUT/pvg_$mode.err"; r=$?
  grep -E "^(FAIL|PASS test_pvg)" "$OUT/pvg_$mode.out"
  echo "$(grep -c '^PASS' "$OUT/pvg_$mode.out") checks passed, $(grep -c '^FAIL' "$OUT/pvg_$mode.out") failed ($mode)"
  [ $r = 0 ] || { echo "test_pvg ($mode) FAILED (exit $r)"; tail -15 "$OUT/pvg_$mode.err"; rc=1; }
done
exit $rc
