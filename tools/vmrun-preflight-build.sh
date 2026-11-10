#!/bin/bash
# vmrun with the real restore tool in it (--restore-preflight IPSW): host/vmrun.c -DHV_RESTORE_PREFLIGHT + host/restore_preflight.c + the restore stack that
# tools/restore-selftest-build.sh builds (its lib.a: libirecovery, usbmuxd, libimobiledevice, libtatsu, idevicerestore, the HTTP stand-in, the libusb stand-in).
# Run after tools/engine-build.sh, tools/devices-build.sh and tools/restore-selftest-build.sh with the same compiler; <output dir> is the engine's, <restore dir> the
# selftest's. A separate program on purpose: if it does not build, the plain vmrun and everything else still ship.
# Usage: CC=<compiler> [EXTRA_LIBS=...] [PF_THREADLIB="" on Android: bionic has no libpthread] tools/vmrun-preflight-build.sh <output dir> <restore build dir>
set -u
OUT=${1:?output dir}
RS=${2:?restore build dir}
: "${CC:?CC}"
TP=third_party
LIB="$RS/rs/lib.a"; ZIP="$RS/rs/libzip-build/lib/libzip.a"
[ -f "$LIB" ] && [ -f "$ZIP" ] || { echo "FAILED  the restore stack is not built in $RS (tools/restore-selftest-build.sh)"; exit 1; }
FL="-std=gnu11 -D_GNU_SOURCE -O2 -g -w -DHAVE_CONFIG_H -Icore -Ihost -Idevices -I$TP/restore_config -I$TP -I$TP/libusb-1.0 -I$TP/curl_include -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libirecovery/include -I$TP/idevicerestore/src -I$RS/rs/libzip-build -I$TP/libzip/lib"
R="$OUT/preflight"
mkdir -p "$R"
$CC $FL -c host/restore_preflight.c -o "$R/restore_preflight.o" 2> "$R/pf.log" || { echo "FAILED  host/restore_preflight.c"; head -20 "$R/pf.log"; exit 1; }
$CC $FL -DHV_RESTORE_PREFLIGHT -c host/vmrun.c -o "$R/vmrun.o" 2> "$R/vmrun.o.log" || { echo "FAILED  host/vmrun.c"; head -20 "$R/vmrun.o.log"; exit 1; }
eobjs=""; for o in classify operands translate dispatch mmu memop exception sysreg machine jit_region disk enter; do eobjs="$eobjs $OUT/$o.o"; done
dobjs="$(ls $OUT/dev_*.o)"
vobjs="$OUT/vm_vm.o $OUT/vm_stubs_portable.o $OUT/vm_usb_host.o"
if $CC $FL "$R/vmrun.o" "$R/restore_preflight.o" $vobjs $eobjs $dobjs "$LIB" "$ZIP" ${EXTRA_LDFLAGS:-} ${EXTRA_LIBS:-} -lz -lm ${PF_THREADLIB--lpthread} -o "$OUT/vmrun_preflight" 2> "$R/link.log"; then
  echo "linked  $OUT/vmrun_preflight"
else
  echo "FAILED  link vmrun_preflight"; grep -o "undefined reference to .*" "$R/link.log" | sort -u | head -30; head -30 "$R/link.log" | cut -c1-200; exit 1
fi
