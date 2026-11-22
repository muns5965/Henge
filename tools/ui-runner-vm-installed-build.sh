#!/bin/bash
# C1-2 INSTALLED: builds the installed-disk VM runner (host/ui_runner_vm_installed.c) against the REAL engine, the same way
# tools/ui-runner-vm-build.sh builds host/ui_runner_vm.c -- same engine/device objects, same host/runner_vm_core.c, host/frame_shm.c,
# host/display_buf.c. Then the static safety check (tools/check-runner-vm-installed-safe.sh), this binary's own counterpart of
# tools/check-runner-vm-safe.sh. Linux arm64 / Android arm64 only (the engine's assembly is arm64).
# EXPECT_ARCH defaults to AArch64 (the ELF machine the static check requires).
# Usage: CC=<compiler> [NM=<nm>] [READELF=<readelf>] [EXTRA_LIBS=...] [PF_THREADLIB="" on Android: bionic has no libpthread] tools/ui-runner-vm-installed-build.sh <engine dir> [<output dir>]
set -u
cd "$(dirname "$0")/.."
E=${1:?engine build dir (after tools/engine-build.sh and tools/devices-build.sh)}; OUT=${2:-$E}; CC=${CC:?CC}
mkdir -p "$OUT/uirvmi"
eobjs=""; for o in classify operands translate dispatch mmu memop exception sysreg machine jit_region disk enter; do eobjs="$eobjs $E/$o.o"; done
dobjs="$(ls $E/dev_*.o)"
vobjs="$E/vm_vm.o $E/vm_stubs_portable.o $E/vm_usb_host.o"
for o in $eobjs $vobjs; do [ -f "$o" ] || { echo "FAILED  missing $o (run the engine and devices builds first)"; exit 1; }; done
CF="-std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -Icore -Ihost -Idevices"
srcs="host/ui_runner_vm_installed.c host/runner_vm_core.c host/frame_shm.c host/display_buf.c"
objs=""
for f in $srcs; do
  o="$OUT/uirvmi/$(basename "$f" .c).o"
  $CC $CF -c "$f" -o "$o" 2> "$o.log" || { echo "FAILED  $f"; head -30 "$o.log"; exit 1; }
  objs="$objs $o"
done
if $CC $CF $objs $vobjs $eobjs $dobjs ${EXTRA_LDFLAGS:-} ${EXTRA_LIBS:-} -lm ${PF_THREADLIB--lpthread} -o "$OUT/ui_runner_vm_installed" 2> "$OUT/uirvmi/link.log"; then
  echo "linked  $OUT/ui_runner_vm_installed"
else
  echo "FAILED  link ui_runner_vm_installed"; grep -o "undefined reference to .*" "$OUT/uirvmi/link.log" | sort -u | head -30; head -30 "$OUT/uirvmi/link.log" | cut -c1-200; exit 1
fi
file "$OUT/ui_runner_vm_installed" 2>/dev/null || true
EXPECT_ARCH=${EXPECT_ARCH-AArch64} tools/check-runner-vm-installed-safe.sh "$OUT/ui_runner_vm_installed"
