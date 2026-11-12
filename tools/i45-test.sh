#!/bin/bash
# tests/test_i45.c: I4 (restored_send lock), I5 (idevice connection registration/references), fd reuse. Linux. Usage: [CC=cc] [SAN=address|thread] tools/i45-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}; TP=third_party; R="$OUT/rs"; SAN=${SAN:-}
[ -f "$R/lib.a" ] || CC="$CC${SAN:+ -fsanitize=$SAN}" tools/restore-selftest-build.sh "$OUT" > "$OUT.build.log" 2>&1 || { echo "FAILED restore stack build"; exit 1; }
mkdir -p "$OUT/i45"
INC="-DHAVE_MBEDTLS -I$TP/mbedtls/include -DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libusbmuxd/include -I$TP/libimobiledevice/include -I$TP/libimobiledevice -I$TP/libimobiledevice/src -I$TP/libirecovery/include -I$R/libzip-build -I$TP/libzip/lib -I$TP/idevicerestore/src -I. -Ihost -Idevices -Icore -Itests"
$CC ${SAN:+-fsanitize=$SAN} -std=gnu11 -D_GNU_SOURCE -O1 -g -w $INC tests/test_i45.c tests/fake_device/fake_dev.c tests/fake_device/stubs.c "$R/lib.a" "$R/libzip-build/lib/libzip.a" -lz -lm -lpthread -o "$OUT/i45/t" 2> "$OUT/i45/link.log" || { echo "FAILED build"; head -20 "$OUT/i45/link.log"; exit 1; }
rc=0
for c in ${CASES:-null reg wake refwait refleak reuse t4}; do
  ASAN_OPTIONS="detect_leaks=0" TSAN_OPTIONS="halt_on_error=0 report_thread_leaks=0" timeout 120 "$OUT/i45/t" $c > "$OUT/i45/$c.out" 2> "$OUT/i45/$c.err"; r=$?
  echo "$c: exit $r, $(grep -c ^PASS "$OUT/i45/$c.out") pass, $(grep -c ^FAIL "$OUT/i45/$c.out") fail"; grep -E "^(FAIL|INFO)" "$OUT/i45/$c.out"
  grep -E 'ERROR: AddressSanitizer|WARNING: ThreadSanitizer' "$OUT/i45/$c.err" | sort | uniq -c | head -3
  [ $r = 0 ] || rc=1
done
[ $rc = 0 ] && echo "I45 OK" || echo "I45 FAILED"; exit $rc
