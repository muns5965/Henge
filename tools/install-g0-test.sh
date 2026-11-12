#!/bin/bash
# tests/test_install_g0.c: host/install_g0.c + host/install.c (both -D__ANDROID__ -DHV_INSTALL_G0 -DHV_STOP_BEFORE_ROOTFS on Linux) with the real session, registry and
# connection policy; link-time wrappers for usbmuxd_main, idevicerestore_start and hv_usb_wait_online. Needs the restore stack (built when missing).
# Usage: [CC=cc] tools/install-g0-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}; TP=third_party; R="$OUT/rs"
[ -f "$R/lib.a" ] || tools/restore-selftest-build.sh "$OUT" > "$OUT.build.log" 2>&1 || { echo "FAILED restore stack build"; exit 1; }
mkdir -p "$OUT/g0"
INC="-DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libusbmuxd/include -I$TP/libimobiledevice/include -I$TP/libirecovery/include -I$R/libzip-build -I$TP/libzip/lib -I$TP/libtatsu/include -I$TP/idevicerestore/src -I. -Icore -Idevices -Ihost"
FLG="-std=gnu11 -D_GNU_SOURCE -O1 -g -w $INC -D__ANDROID__ -DHV_INSTALL_G0 -DHV_STOP_BEFORE_ROOTFS"
cc $FLG -c tests/test_install_g0.c -o "$OUT/g0/test.o" || { echo "FAILED build test"; exit 1; }
$CC $FLG "$OUT/g0/test.o" host/install.c host/install_g0.c host/disk.c host/crypto_portable.c tests/fake_device/stubs.c "$R/lib.a" "$R/libzip-build/lib/libzip.a" \
  -Wl,--wrap=usbmuxd_main -Wl,--wrap=idevicerestore_start -Wl,--wrap=hv_usb_wait_online -lz -lm -lpthread -o "$OUT/g0/t" 2> "$OUT/g0/link.log" || { echo "FAILED build"; grep -o "undefined reference to .*\|multiple definition.*" "$OUT/g0/link.log" | sort -u | head; head -20 "$OUT/g0/link.log"; exit 1; }
rc=0
for c in ${CASES:-pass disk_changed aux_changed no_hook ran_on write_asked meta_only meta_changed cancel stuck refuse_missing no_ecid readonly_disk}; do
  TO=""; case $c in stuck) TO="report_thread_leaks=0";; esac
  TSAN_OPTIONS="$TO" timeout 120 "$OUT/g0/t" $c > "$OUT/g0/$c.out" 2> "$OUT/g0/$c.err"; r=$?
  echo "$c: exit $r, $(grep -c ^PASS "$OUT/g0/$c.out") pass, $(grep -c ^FAIL "$OUT/g0/$c.out") fail"; grep ^FAIL "$OUT/g0/$c.out"
  [ $r = 0 ] || { rc=1; tail -3 "$OUT/g0/$c.err"; }
done
[ $rc = 0 ] && echo "INSTALL-G0 OK" || echo "INSTALL-G0 FAILED"; exit $rc
