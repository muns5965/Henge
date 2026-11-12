#!/bin/bash
# STAGE 1 evidence runs (tests/test_stage1_blocking.c): partial frames, shutdown() against blocked recv/send/connect, libusbmuxd monitor unsubscribe (+ join failure injection).
# RECORDS behaviour; the exit status only says the cases ran. Linux. Needs the restore stack. Usage: [CC=cc] tools/stage1-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}; TP=third_party; R="$OUT/rs"
[ -f "$R/lib.a" ] || tools/restore-selftest-build.sh "$OUT" > "$OUT.build.log" 2>&1 || { echo "FAILED restore stack build"; exit 1; }
mkdir -p "$OUT/s1"
INC="-DHAVE_MBEDTLS -I$TP/mbedtls/include -DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libusbmuxd/include -I$TP/libimobiledevice/include -I$TP/libimobiledevice -I$TP/libimobiledevice/src -I$TP/libirecovery/include -I$R/libzip-build -I$TP/libzip/lib -I$TP/idevicerestore/src -I. -Ihost -Idevices -Icore -Itests"
$CC -std=gnu11 -D_GNU_SOURCE -O1 -g -w $INC tests/test_stage1_blocking.c tests/fake_device/fake_dev.c tests/fake_device/stubs.c "$R/lib.a" "$R/libzip-build/lib/libzip.a" \
  -Wl,--wrap=thread_join -Wl,--wrap=thread_free -Wl,--wrap=socket_close -Wl,--wrap=thread_new -lz -lm -lpthread -o "$OUT/s1/t" 2> "$OUT/s1/link.log" || { echo "FAILED build"; head -20 "$OUT/s1/link.log"; exit 1; }
for c in ${CASES:-pf_full pf_idle pf_p1 pf_p2 pf_p3 pf_b s1 s5 s1b s2 s3 s3lib s4 s4j s4o s4d s4n s4jj}; do
  echo "=== case $c"
  TSAN_OPTIONS="halt_on_error=0 report_thread_leaks=0" ASAN_OPTIONS="detect_leaks=0" timeout 150 "$OUT/s1/t" $c > "$OUT/s1/$c.out" 2> "$OUT/s1/$c.err"
  echo "exit=$?"
  grep -E '^(PF_|SD_|WATCHDOG)' "$OUT/s1/$c.out"
  grep -E 'ERROR: AddressSanitizer|WARNING: ThreadSanitizer|SUMMARY:' "$OUT/s1/$c.err" | head -4
done
