#!/bin/bash
# The strict control path (host/usb_host.c, devices/virtio_usb.c) against a fake guest. Linux, any compiler, no libusb, no device.
# Each scenario runs in its own process with a time limit: a scenario that does not end is reported as HUNG.
# Usage: [CC=cc] tools/usb-ctrl-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-gcc}
mkdir -p "$OUT"
W="-std=gnu11 -D_GNU_SOURCE -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-result -Wno-unused-function -Icore -Ihost -Idevices"
rc=0
for mode in plain asan; do
  F="-O1"; [ $mode = asan ] && F="-O1 -fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
  $CC $W $F devices/virtio_usb.c host/usb_host.c tests/test_usb_ctrl.c -lpthread -o "$OUT/test_usb_ctrl_$mode" 2> "$OUT/uc_$mode.log" || { echo "FAILED build ($mode)"; head -30 "$OUT/uc_$mode.log"; rc=1; continue; }
  echo "=== usb control ($mode)"
  for sc in basic timeout legacy cancel race concurrent reset session locks cleanup; do
    ASAN_OPTIONS=detect_leaks=0 timeout 60 "$OUT/test_usb_ctrl_$mode" $sc > "$OUT/uc_${mode}_$sc.out" 2> "$OUT/uc_${mode}_$sc.err"; r=$?
    grep -E "^(FAIL|PASS)" "$OUT/uc_${mode}_$sc.out" | cut -c1-260
    if [ $r = 124 ]; then echo "HUNG scenario $sc ($mode)"; rc=1
    elif [ $r != 0 ]; then echo "SCENARIO $sc FAILED ($mode, exit $r)"; tail -12 "$OUT/uc_${mode}_$sc.err"; rc=1; fi
  done
done
exit $rc
