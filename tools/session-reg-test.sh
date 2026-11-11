#!/bin/bash
# tests/test_session_reg.c: the session coupled to the lifetime registry. Linux. Needs the restore stack (tools/restore-selftest-build.sh).
# Usage: [CC=cc] tools/session-reg-test.sh <build dir>
set -u
OUT=${1:?build dir}; CC=${CC:-cc}; TP=third_party; R="$OUT/rs"
[ -f "$R/lib.a" ] || tools/restore-selftest-build.sh "$OUT" > "$OUT.build.log" 2>&1 || { echo "FAILED restore stack build"; exit 1; }
mkdir -p "$OUT/sr"
$CC -std=gnu11 -D_GNU_SOURCE -O1 -g -w -I$TP/idevicerestore/src -I$TP/libimobiledevice/src -I$TP/libplist/include -I$TP -I. -Icore -Idevices -Ihost tests/test_session_reg.c tests/fake_device/stubs.c "$R/lib.a" "$R/libzip-build/lib/libzip.a" -lz -lm -lpthread -o "$OUT/sr/t" 2> "$OUT/sr/link.log" || { echo "FAILED build"; head -20 "$OUT/sr/link.log"; exit 1; }
rc=0
for c in normal stuck engine busy owner xfer abort xfer_race finish pending reentry hang cancel_thread abort_race pubrights life life_cancel; do
  timeout 60 "$OUT/sr/t" $c > "$OUT/sr/$c.out" 2>&1; r=$?
  echo "$c: exit $r, $(grep -c ^PASS "$OUT/sr/$c.out") pass, $(grep -c ^FAIL "$OUT/sr/$c.out") fail"; grep ^FAIL "$OUT/sr/$c.out"
  [ $r = 0 ] || rc=1
done
[ $rc = 0 ] && echo "SESSION-REG OK" || echo "SESSION-REG FAILED"; exit $rc
