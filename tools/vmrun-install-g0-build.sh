#!/bin/bash
# vmrun with the install entry point (host/install.c, the code the app's install will use) and the G0 test in it (--install-g0 IPSW): host/vmrun.c and host/install.c
# with -DHV_INSTALL_G0 -DHV_STOP_BEFORE_ROOTFS, + host/install_g0.c + the restore stack that tools/restore-selftest-build.sh builds. G0 runs the entry point once up to the
# restore tool's own stop A, with the disk and the auxiliary storage read-only (see host/install_g0.h). A test program: it is a separate binary, and neither switch is set
# anywhere else (tools/check-no-test-flags.sh). Named vmrun_g0 on purpose: it is not an installer.
# Run after tools/engine-build.sh, tools/devices-build.sh and tools/restore-selftest-build.sh with the same compiler; <output dir> is the engine's, <restore dir> the
# selftest's. Usage: CC=<compiler> [EXTRA_LIBS=...] [PF_THREADLIB="" on Android: bionic has no libpthread] tools/vmrun-install-g0-build.sh <output dir> <restore build dir>
set -u
OUT=${1:?output dir}
RS=${2:?restore build dir}
: "${CC:?CC}"
TP=third_party
LIB="$RS/rs/lib.a"; ZIP="$RS/rs/libzip-build/lib/libzip.a"
[ -f "$LIB" ] && [ -f "$ZIP" ] || { echo "FAILED  the restore stack is not built in $RS (tools/restore-selftest-build.sh)"; exit 1; }
FL="-std=gnu11 -D_GNU_SOURCE -O2 -g -w -DHAVE_CONFIG_H -Icore -Ihost -Idevices -I$TP/restore_config -I$TP -I$TP/libtatsu/include -I$TP/libusb-1.0 -I$TP/curl_include -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libirecovery/include -I$TP/idevicerestore/src -I$RS/rs/libzip-build -I$TP/libzip/lib -DHV_STOP_BEFORE_ROOTFS -DHV_INSTALL_G0"
R="$OUT/installg0"
mkdir -p "$R"
for f in install install_g0; do
  $CC $FL -c host/$f.c -o "$R/$f.o" 2> "$R/$f.log" || { echo "FAILED  host/$f.c"; head -20 "$R/$f.log"; exit 1; }
done
$CC $FL -c host/vmrun.c -o "$R/vmrun.o" 2> "$R/vmrun.o.log" || { echo "FAILED  host/vmrun.c"; head -20 "$R/vmrun.o.log"; exit 1; }
# the disk object and vm.o of the engine build are reused: they were built with the same flags minus the two switches, which neither of them reads
eobjs=""; for o in classify operands translate dispatch mmu memop exception sysreg machine jit_region disk enter; do eobjs="$eobjs $OUT/$o.o"; done
dobjs="$(ls $OUT/dev_*.o)"
vobjs="$OUT/vm_vm.o $OUT/vm_stubs_portable.o $OUT/vm_usb_host.o"
if $CC $FL "$R/vmrun.o" "$R/install.o" "$R/install_g0.o" $vobjs $eobjs $dobjs "$LIB" "$ZIP" ${EXTRA_LDFLAGS:-} ${EXTRA_LIBS:-} -lz -lm ${PF_THREADLIB--lpthread} -o "$OUT/vmrun_g0" 2> "$R/link.log"; then
  echo "linked  $OUT/vmrun_g0"
else
  echo "FAILED  link vmrun_g0"; grep -o "undefined reference to .*" "$R/link.log" | sort -u | head -30; head -30 "$R/link.log" | cut -c1-200; exit 1
fi
