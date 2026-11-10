#!/bin/bash
# host/dnload_probe.c against a stand-in for the ROM's USB side (24 scenarios: A0-A2 and A0-A3). Linux, any compiler. No ROM, no phone, nothing real is sent anywhere.
# Usage: [CC=cc] tools/dnload-test.sh <build dir>
set -u
OUT=${1:?build dir}
CC=${CC:-cc}
mkdir -p "$OUT"
FL="-std=gnu11 -D_GNU_SOURCE -O1 -g -w -Werror=implicit-function-declaration -Ihost -Idevices -Icore"
objs=""
for f in host/dnload_probe.c devices/virtio_usb.c host/usb_host.c; do
  o="$OUT/$(basename "${f%.*}").o"
  $CC $FL -c "$f" -o "$o" 2> "$o.log" || { echo "FAILED  $f"; head -20 "$o.log"; exit 1; }
  objs="$objs $o"
done
$CC $FL tests/test_dnload.c $objs -lpthread -o "$OUT/test_dnload" 2> "$OUT/link.log" || { echo "FAILED  link"; head -30 "$OUT/link.log"; exit 1; }
rc=0
for sc in sync idle-after error-after ignored not-idle stall-dnload silent-dnload stall-state stall-setcfg configured no-device a3-ok a3-err-status a3-poll0 a3-pollbig a3-stall a3-short a3-silent a3-state3 a3-ignored a3-error-state a3-not-idle a3-stall-dnload a3-stall-state; do
  timeout 60 "$OUT/test_dnload" $sc > "$OUT/$sc.out" 2> "$OUT/$sc.err"; r=$?
  if [ $r != 0 ]; then echo "FAIL $sc (exit $r)"; cat "$OUT/$sc.out" | tail -25; rc=1; else echo "PASS $sc: $(grep '^scenario' "$OUT/$sc.out" | cut -c1-200)"; fi
done
exit $rc
