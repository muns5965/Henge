#!/bin/sh
# Build the device models for a non-Apple target and try to link tests/test_devices.c.
# Run after tools/engine-build.sh with the same compiler and output dir (it reuses the engine objects there).
# Usage: CC=<compiler> [EXTRA_LIBS=...] tools/devices-build.sh <output dir>
# Every file is compiled on its own so one run lists everything that does not build. The graphics device runs
# against host/pvg_metal_stub.c, which draws nothing: a link here says the devices fit together, not that a
# GPU or a guest works.
set -u
OUT=${1:?output dir}
: "${CC:?CC}"
CFLAGS="-std=gnu11 -D_GNU_SOURCE -Wall -Wextra -O2 -g -Icore -Ihost -Idevices -Wno-unused-parameter"
fail=0
dobjs=""
for f in devices/*.c host/crypto_portable.c host/pvg_metal_stub.c; do
  o="$OUT/dev_$(basename "$f" .c).o"
  if $CC $CFLAGS -c "$f" -o "$o" 2> "$o.log"; then
    echo "ok      $f"; cat "$o.log"; dobjs="$dobjs $o"
  else
    echo "FAILED  $f"; cat "$o.log"; fail=1
  fi
done
[ $fail = 0 ] || { echo "device compile step failed; not linking"; exit 1; }
eobjs=""
for o in classify operands translate dispatch mmu memop exception sysreg machine jit_region disk enter; do eobjs="$eobjs $OUT/$o.o"; done
$CC $CFLAGS -march=armv8.5-a -Itests -c tests/devices_programs.S -o "$OUT/devices_programs.o" || { echo "FAILED  tests/devices_programs.S"; exit 1; }
if $CC $CFLAGS -Itests tests/test_devices.c "$OUT/devices_programs.o" "$OUT/harness.o" $eobjs $dobjs ${EXTRA_LIBS:-} -o "$OUT/test_devices" 2> "$OUT/test_devices.log"; then
  echo "linked  $OUT/test_devices"
else
  echo "FAILED  link test_devices"; cat "$OUT/test_devices.log"; exit 1
fi
