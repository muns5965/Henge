#!/bin/bash
# Failure injection for the virtual USB data path (devices/virtio_usb.c + host/libusb_shim.c) against a fake guest. Linux, any compiler, no libusb, no device.
# One process per scenario with a time limit (HUNG if it does not end). Usage: [CC=cc] tools/usb-faults-test.sh <build dir> [source dir]
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; SRC=${2:-.}; CC=${CC:-gcc}
mkdir -p "$OUT"
W="-std=gnu11 -D_GNU_SOURCE -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-result -Wno-unused-function -Wno-missing-field-initializers -I$SRC/core -I$SRC/host -I$SRC/devices -Ithird_party -Ithird_party/libusb-1.0"
HAS=""; grep -q hv_vusb_take_lost "$SRC/devices/virtio_usb.h" && HAS="-DHAS_LOST"
rc=0
for mode in ${MODES:-plain asan}; do
  F="-O1"; [ $mode = tsan ] && F="-O1 -fsanitize=thread -fno-omit-frame-pointer"; [ $mode = asan ] && F="-O1 -fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
  $CC $W $F $HAS -Wl,--wrap=pthread_create "$SRC/devices/virtio_usb.c" "$SRC/host/usb_host.c" "$SRC/host/libusb_shim.c" tests/test_usb_faults.c -lpthread -o "$OUT/test_usb_faults_$mode" 2> "$OUT/uf_$mode.log" || { echo "FAILED build ($mode)"; head -30 "$OUT/uf_$mode.log"; rc=1; continue; }
  echo "=== usb faults ($mode)"
  for sc in in_full out_room lost deadline late_ack out_partial cancel_next queue_wait0 session debt_lost no_spam loss_log workers; do
    ASAN_OPTIONS=detect_leaks=0 timeout 60 "$OUT/test_usb_faults_$mode" $sc > "$OUT/uf_${mode}_$sc.out" 2> "$OUT/uf_${mode}_$sc.err"; r=$?
    grep -E "^(FAIL|PASS|note)" "$OUT/uf_${mode}_$sc.out" | cut -c1-260
    if [ $r = 124 ]; then echo "HUNG scenario $sc ($mode)"; rc=1
    elif [ $r != 0 ]; then echo "SCENARIO $sc FAILED ($mode, exit $r)"; rc=1; fi
  done
done
exit $rc
