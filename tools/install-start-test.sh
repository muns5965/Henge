#!/bin/bash
# tests/test_install_start.c: host/install.c compiled -D__ANDROID__ on Linux, with link-time wrappers (pthread_create, usbmuxd_main, idevicerestore_start). Needs the restore stack.
# Usage: [CC=cc] tools/install-start-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}; TP=third_party; R="$OUT/rs"
[ -f "$R/lib.a" ] || tools/restore-selftest-build.sh "$OUT" > "$OUT.build.log" 2>&1 || { echo "FAILED restore stack build"; exit 1; }
mkdir -p "$OUT/is"
INC="-DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libusbmuxd/include -I$TP/libimobiledevice/include -I$TP/libirecovery/include -I$R/libzip-build -I$TP/libzip/lib -I$TP/libtatsu/include -I$TP/idevicerestore/src -I. -Icore -Idevices -Ihost"
# the test file itself is built WITHOUT sanitizers (its own flags and counters are plain variables); host/install.c, the session, the registry and the stack are built with $CC
cc -std=gnu11 -D_GNU_SOURCE -O1 -g -w $INC -D__ANDROID__ -c tests/test_install_start.c -o "$OUT/is/test.o" || { echo "FAILED build test"; exit 1; }
$CC -std=gnu11 -D_GNU_SOURCE -O1 -g -w $INC -D__ANDROID__ "$OUT/is/test.o" host/install.c tests/fake_device/stubs.c "$R/lib.a" "$R/libzip-build/lib/libzip.a" \
  -Wl,--wrap=pthread_create -Wl,--wrap=usbmuxd_main -Wl,--wrap=idevicerestore_start -lz -lm -lpthread -o "$OUT/is/t" 2> "$OUT/is/link.log" || { echo "FAILED build"; head -20 "$OUT/is/link.log"; exit 1; }
rc=0
for c in ${CASES:-ok p1 p2 p2_stuck end_fail cancel}; do
  TO=""; case $c in p2_stuck|end_fail) TO="report_thread_leaks=0";; esac   # these two leave the usbmuxd thread running ON PURPOSE (C11: a thread that did not stop is kept, not forced)
  TSAN_OPTIONS="$TO" timeout 90 "$OUT/is/t" $c > "$OUT/is/$c.out" 2> "$OUT/is/$c.err"; r=$?
  echo "$c: exit $r, $(grep -c ^PASS "$OUT/is/$c.out") pass, $(grep -c ^FAIL "$OUT/is/$c.out") fail"; grep ^FAIL "$OUT/is/$c.out"
  [ $r = 0 ] || { rc=1; tail -3 "$OUT/is/$c.err"; }
done
[ $rc = 0 ] && echo "INSTALL-START OK" || echo "INSTALL-START FAILED"; exit $rc
