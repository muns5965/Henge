#!/bin/bash
# S1 baseline: the current restore_device() against a fake usbmuxd + fake restored on loopback (tests/test_fake_restore.c, tests/fake_device/). Linux. Needs the restore stack
# (tools/restore-selftest-build.sh). RECORDS results; the exit status only says the cases ran, never that restore_device behaved well.
# Sanitizer runs: build the stack in another dir with CC="cc -fsanitize=address -g" (or thread) and pass the same CC here; their reports land in case_<id>.log.
# Usage: [CC=cc] [CASES="0 a b c1 c2 c3 d e t5s_bb t5s_fdr t5s_fdr_nr t5a_free t5a_nofree t12 t12_close t13 t1b t3_inject"] [FAKE_CASE_LIMIT_S=150] tools/fake-restore-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}
CC=${CC:-cc}
TP=third_party
R="$OUT/rs"
[ -f "$R/lib.a" ] || tools/restore-selftest-build.sh "$OUT" > "$OUT.build.log" 2>&1 || { echo "FAILED  restore stack build"; tail -20 "$OUT.build.log"; exit 1; }
mkdir -p "$OUT/fd"
INC="-DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libusbmuxd/include -I$TP/libimobiledevice/include -I$TP/libirecovery/include -I$R/libzip-build -I$TP/libzip/lib -I$TP/idevicerestore/src -I. -Ihost -Idevices -Icore -Itests"
$CC -std=gnu11 -D_GNU_SOURCE -O1 -g -w $INC tests/test_fake_restore.c tests/fake_device/fake_dev.c tests/fake_device/stubs.c tests/fake_device/wrap_thread.c -Wl,--wrap=thread_new "$R/lib.a" "$R/libzip-build/lib/libzip.a" -lz -lm -lpthread -o "$OUT/fd/test_fake_restore" 2> "$OUT/fd/link.log" \
  || { echo "FAILED  build test_fake_restore"; head -30 "$OUT/fd/link.log"; exit 1; }
for c in ${CASES:-0 a b c1 c2 c3 d e t5s_bb t5s_fdr t5s_fdr_nr t5a_free t5a_nofree t12 t12_close t13 t1b t3_inject}; do
  echo "=== case $c"
  ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" TSAN_OPTIONS=halt_on_error=0 "$OUT/fd/test_fake_restore" "$c" > "$OUT/fd/case_$c.out" 2> "$OUT/fd/case_$c.log"
  echo "exit=$?"
  grep -E '^(CASE|BUILD|RETURNED|CLIENT|WAITING|HANG|AT_HANG|RESULT|NETGUARD|FAKE|FDS|THREAD_NEW|LATE_SEND|LEAKCHECK|ENGINE)' "$OUT/fd/case_$c.out"
  grep -E 'ERROR: AddressSanitizer|WARNING: ThreadSanitizer|SUMMARY:' "$OUT/fd/case_$c.log" | head -6
done
