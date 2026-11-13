#!/bin/bash
# The strict control path (host/usb_host.c, devices/virtio_usb.c) against a fake guest. Linux, any compiler, no libusb, no device.
# Each scenario runs in its own process with a time limit: a scenario that does not end is reported as HUNG.
# Usage: [CC=cc] tools/usb-ctrl-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-gcc}
mkdir -p "$OUT"
W="-std=gnu11 -D_GNU_SOURCE -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-result -Wno-unused-function -Icore -Ihost -Idevices -Ithird_party -Ithird_party/libusb-1.0"
rc=0
for mode in plain asan; do
  F="-O1"; [ $mode = asan ] && F="-O1 -fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
  $CC $W $F devices/virtio_usb.c host/usb_host.c tests/test_usb_ctrl.c -lpthread -o "$OUT/test_usb_ctrl_$mode" 2> "$OUT/uc_$mode.log" || { echo "FAILED build ($mode)"; head -30 "$OUT/uc_$mode.log"; rc=1; continue; }
  $CC $W $F -Wno-missing-field-initializers devices/virtio_usb.c host/usb_host.c host/libusb_shim.c tests/test_shim_ctrl.c -lpthread -o "$OUT/test_shim_ctrl_$mode" 2> "$OUT/sh_$mode.log" || { echo "FAILED build shim test ($mode)"; head -30 "$OUT/sh_$mode.log"; rc=1; }
  echo "=== usb control ($mode)"
  for sc in basic timeout legacy cancel race concurrent reset session locks cleanup contract modeflip clock; do
    ASAN_OPTIONS=detect_leaks=0 timeout 60 "$OUT/test_usb_ctrl_$mode" $sc > "$OUT/uc_${mode}_$sc.out" 2> "$OUT/uc_${mode}_$sc.err"; r=$?
    grep -E "^(FAIL|PASS)" "$OUT/uc_${mode}_$sc.out" | cut -c1-260
    if [ $r = 124 ]; then echo "HUNG scenario $sc ($mode)"; rc=1
    elif [ $r != 0 ]; then echo "SCENARIO $sc FAILED ($mode, exit $r)"; tail -12 "$OUT/uc_${mode}_$sc.err"; rc=1; fi
  done
  if [ -x "$OUT/test_shim_ctrl_$mode" ]; then
    echo "=== shim transfers over strict control ($mode)"
    for sc in sync cancel_run cancel_queued race sync_timeout legacy lifetime close_run close_queued close_done dup submit_fail cbfree balance; do
      ASAN_OPTIONS=detect_leaks=0 timeout 100 "$OUT/test_shim_ctrl_$mode" $sc > "$OUT/sh_${mode}_$sc.out" 2> "$OUT/sh_${mode}_$sc.err"; r=$?
      grep -E "^(FAIL|PASS)" "$OUT/sh_${mode}_$sc.out" | cut -c1-300
      if [ $r = 124 ]; then echo "HUNG shim scenario $sc ($mode)"; rc=1
      elif [ $r != 0 ]; then echo "SHIM SCENARIO $sc FAILED ($mode, exit $r)"; tail -14 "$OUT/sh_${mode}_$sc.err"; rc=1; fi
    done
  fi
done
exit $rc
