#!/bin/bash
# Step 3 tests (tests/test_client_release.c): per-client async tracker and idevicerestore_client_release. Linux. Usage: [CC=cc] tools/client-release-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}; TP=third_party; R="$OUT/rs"
[ -f "$R/lib.a" ] || tools/restore-selftest-build.sh "$OUT" > "$OUT.build.log" 2>&1 || { echo "FAILED restore stack build"; exit 1; }
mkdir -p "$OUT/cr"
INC="-DHAVE_MBEDTLS -I$TP/mbedtls/include -DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libusbmuxd/include -I$TP/libimobiledevice/include -I$TP/libimobiledevice -I$TP/libimobiledevice/src -I$TP/libirecovery/include -I$R/libzip-build -I$TP/libzip/lib -I$TP/idevicerestore/src -I. -Ihost -Idevices -Icore -Itests"
$CC -std=gnu11 -D_GNU_SOURCE -O1 -g -w $INC tests/test_client_release.c tests/fake_device/stubs.c "$R/lib.a" "$R/libzip-build/lib/libzip.a" -lz -lm -lpthread -o "$OUT/cr/t" 2> "$OUT/cr/link.log" || { echo "FAILED build"; head -20 "$OUT/cr/link.log"; exit 1; }
for c in r_null r_race r_block r_closed; do
  echo "=== case $c"
  TSAN_OPTIONS="halt_on_error=0 report_thread_leaks=0" ASAN_OPTIONS="detect_leaks=0" timeout 120 "$OUT/cr/t" $c > "$OUT/cr/$c.out" 2> "$OUT/cr/$c.err"
  echo "exit=$?"
  grep -E '^CR_' "$OUT/cr/$c.out"
  grep -E 'ERROR: AddressSanitizer|WARNING: ThreadSanitizer|SUMMARY:' "$OUT/cr/$c.err" | head -4
done
