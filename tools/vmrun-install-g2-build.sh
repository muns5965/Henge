#!/bin/bash
# vmrun with the install entry point (host/install.c, the code the app's install will use) and the G2 diagnostic in it (--install-g2 IPSW): host/vmrun.c and
# host/install.c with -DHV_INSTALL_G2, + host/install_g2.c + the restore stack that tools/restore-selftest-build.sh builds. G2 runs the entry point once, up to and
# including the real idevicerestore TSS stage against Apple's real signing server (FLAG_SHSHONLY: no root filesystem step, nothing sent to the device), with the
# disk and the auxiliary storage read-only (see host/install_g2.h). The linked restore stack was itself compiled with -DHV_STOP_BEFORE_ROOTFS (tools/restore-
# selftest-build.sh, used unchanged): a second, independent stop baked into the vendored tool itself, in case FLAG_SHSHONLY ever failed to apply. A diagnostic
# program: it is a separate binary, and -DHV_INSTALL_G2 is set nowhere else (tools/check-no-test-flags.sh). Named vmrun_g2 on purpose: it is not an installer.
# Run after tools/engine-build.sh, tools/devices-build.sh and tools/restore-selftest-build.sh with the same compiler; <output dir> is the engine's, <restore dir> the
# selftest's. Usage: CC=<compiler> [EXTRA_LIBS=...] [PF_THREADLIB="" on Android: bionic has no libpthread] tools/vmrun-install-g2-build.sh <output dir> <restore build dir>
set -u
OUT=${1:?output dir}
RS=${2:?restore build dir}
: "${CC:?CC}"
TP=third_party
LIB="$RS/rs/lib.a"; ZIP="$RS/rs/libzip-build/lib/libzip.a"
[ -f "$LIB" ] && [ -f "$ZIP" ] || { echo "FAILED  the restore stack is not built in $RS (tools/restore-selftest-build.sh)"; exit 1; }
FL="-std=gnu11 -D_GNU_SOURCE -O2 -g -w -DHAVE_CONFIG_H -Icore -Ihost -Idevices -I$TP/restore_config -I$TP -I$TP/libtatsu/include -I$TP/libusb-1.0 -I$TP/curl_include -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libirecovery/include -I$TP/idevicerestore/src -I$RS/rs/libzip-build -I$TP/libzip/lib -DHV_INSTALL_G2"
R="$OUT/installg2"
mkdir -p "$R"
for f in install install_g2; do
  $CC $FL -c host/$f.c -o "$R/$f.o" 2> "$R/$f.log" || { echo "FAILED  host/$f.c"; head -20 "$R/$f.log"; exit 1; }
done
$CC $FL -c host/vmrun.c -o "$R/vmrun.o" 2> "$R/vmrun.o.log" || { echo "FAILED  host/vmrun.c"; head -20 "$R/vmrun.o.log"; exit 1; }
# the disk object and vm.o of the engine build are reused: they were built with the same flags minus the switch, which neither of them reads
eobjs=""; for o in classify operands translate dispatch mmu memop exception sysreg machine jit_region disk enter; do eobjs="$eobjs $OUT/$o.o"; done
dobjs="$(ls $OUT/dev_*.o)"
vobjs="$OUT/vm_vm.o $OUT/vm_stubs_portable.o $OUT/vm_usb_host.o"
if $CC $FL "$R/vmrun.o" "$R/install.o" "$R/install_g2.o" $vobjs $eobjs $dobjs "$LIB" "$ZIP" ${EXTRA_LDFLAGS:-} ${EXTRA_LIBS:-} -lz -lm ${PF_THREADLIB--lpthread} -o "$OUT/vmrun_g2" 2> "$R/link.log"; then
  echo "linked  $OUT/vmrun_g2"
else
  echo "FAILED  link vmrun_g2"; grep -o "undefined reference to .*" "$R/link.log" | sort -u | head -30; head -30 "$R/link.log" | cut -c1-200; exit 1
fi
