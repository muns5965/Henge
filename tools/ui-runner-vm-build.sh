#!/bin/bash
# C1-2: builds the VM runner (host/ui_runner_vm.c) against the REAL engine: the objects tools/engine-build.sh and tools/devices-build.sh left in <engine dir>
# (the engine, the device models, host/vm.c with its portable stand-ins). Then the static safety check (tools/check-runner-vm-safe.sh). Linux arm64 / Android arm64 only
# (the engine's assembly is arm64). Not packed into the APK: this is a CI binary for now.
# Usage: CC=<compiler> [NM=<nm>] [READELF=<readelf>] [EXTRA_LIBS=...] [PF_THREADLIB="" on Android: bionic has no libpthread] tools/ui-runner-vm-build.sh <engine dir> [<output dir>]
set -u
cd "$(dirname "$0")/.."
E=${1:?engine build dir (after tools/engine-build.sh and tools/devices-build.sh)}; OUT=${2:-$E}; CC=${CC:?CC}
mkdir -p "$OUT/uirvm"
eobjs=""; for o in classify operands translate dispatch mmu memop exception sysreg machine jit_region disk enter; do eobjs="$eobjs $E/$o.o"; done
dobjs="$(ls $E/dev_*.o)"
vobjs="$E/vm_vm.o $E/vm_stubs_portable.o $E/vm_usb_host.o"
for o in $eobjs $vobjs; do [ -f "$o" ] || { echo "FAILED  missing $o (run the engine and devices builds first)"; exit 1; }; done
CF="-std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -Icore -Ihost -Idevices"
srcs="host/ui_runner_vm.c host/runner_vm_core.c host/frame_shm.c host/display_buf.c"
objs=""
for f in $srcs; do
  o="$OUT/uirvm/$(basename "$f" .c).o"
  $CC $CF -c "$f" -o "$o" 2> "$o.log" || { echo "FAILED  $f"; head -30 "$o.log"; exit 1; }
  objs="$objs $o"
done
if $CC $CF $objs $vobjs $eobjs $dobjs ${EXTRA_LDFLAGS:-} ${EXTRA_LIBS:-} -lm ${PF_THREADLIB--lpthread} -o "$OUT/ui_runner_vm" 2> "$OUT/uirvm/link.log"; then
  echo "linked  $OUT/ui_runner_vm"
else
  echo "FAILED  link ui_runner_vm"; grep -o "undefined reference to .*" "$OUT/uirvm/link.log" | sort -u | head -30; head -30 "$OUT/uirvm/link.log" | cut -c1-200; exit 1
fi
tools/check-runner-vm-safe.sh "$OUT/ui_runner_vm"
