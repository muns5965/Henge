#!/bin/bash
# The rate limit of the virtual USB device's diagnostic lines (devices/usb_diag_gate.h), plain and TSan. Usage: tools/usb-diag-gate-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; mkdir -p "$OUT"; rc=0
for mode in plain tsan; do
  F="-O1"; [ $mode = tsan ] && F="-O1 -fsanitize=thread -fno-omit-frame-pointer"
  gcc $F -Wall -Wextra -Werror tests/test_usb_diag_gate.c -lpthread -o "$OUT/udg_$mode" 2> "$OUT/udg_$mode.log" || { echo "FAILED build ($mode)"; head -20 "$OUT/udg_$mode.log"; exit 1; }
  echo "=== usb diag gate ($mode)"; "$OUT/udg_$mode" || rc=1
done
# the device compiles with the gate
gcc -O1 -Wall -Wextra -Werror -c devices/bdif.c -Idevices -Icore -Ihost -o "$OUT/bdif.o" 2> "$OUT/bdif.log" || { echo "FAILED bdif.c"; head -20 "$OUT/bdif.log"; exit 1; }
# every per-transaction line of the device goes through the gate
n=$(grep -c 'hv_usb_diag_pass' devices/bdif.c); [ "$n" -ge 5 ] || { echo "FAIL the device does not use the gate everywhere ($n uses)"; rc=1; }
exit $rc
