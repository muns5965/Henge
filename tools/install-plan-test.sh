#!/bin/bash
# G2-b: the mock install orchestrator (host/install_plan.c) against a fake world, plus the isolation check. Linux, any compiler, no network, no device.
# Usage: [CC=cc] tools/install-plan-test.sh <build dir>
set -u
OUT=${1:?build dir}
CC=${CC:-cc}
mkdir -p "$OUT"
rc=0
tools/check-install-plan-isolation.sh || rc=1
for mode in plain asan; do
  W="-std=gnu11 -O1 -g -Wall -Wextra -Werror"; [ $mode = asan ] && W="$W -fsanitize=address,undefined -fno-sanitize=alignment -fno-omit-frame-pointer"
  $CC $W host/install_plan.c tests/test_install_plan.c -o "$OUT/test_install_plan_$mode" 2> "$OUT/ip_$mode.log" || { echo "FAILED build ($mode)"; head -30 "$OUT/ip_$mode.log"; rc=1; continue; }
  ASAN_OPTIONS=detect_leaks=0 timeout 60 "$OUT/test_install_plan_$mode" all > "$OUT/ip_$mode.out" 2> "$OUT/ip_$mode.err"; r=$?
  echo "=== G2-b ($mode)"; grep -E "^(FAIL|PASS test_install_plan)" "$OUT/ip_$mode.out"
  echo "$(grep -c '^PASS' "$OUT/ip_$mode.out") checks passed ($mode)"
  [ $r = 0 ] || { echo "test_install_plan ($mode) FAILED (exit $r)"; tail -15 "$OUT/ip_$mode.err"; rc=1; }
done
exit $rc
