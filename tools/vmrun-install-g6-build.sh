#!/bin/bash
# vmrun with the install entry point (host/install.c, the code the app's install will use) and the G6 diagnostic in it (--install-g6 IPSW): host/vmrun.c and
# host/install.c with -DHV_INSTALL_G6, + host/install_g6.c + the restore stack that tools/restore-selftest-build.sh builds. G6 runs the entry point once,
# through the real idevicerestore TSS stage, the DFU->Recovery personalized firmware transfer, RecoveryOS entry and the restored handshake/queries (the
# same path G2/G3/G4/G5 already cover) and then continues into the Reverse Proxy attempt (this project's builds always define HAVE_REVERSE_PROXY --
# third_party/restore_config/config.h -- so this is the branch that actually compiles and runs; the FDR-listener-thread branch never does) and the
# restore-options plist construction -- stopping right after via the vendored tool's own "stop E" (third_party/idevicerestore/src/hv_stop.h), strictly
# BEFORE restored_start_restore() (the actual disk-writing restore), with the disk and the auxiliary storage read-only (see host/install_g6.h). Full
# design: docs/g6-stop-e-design-2026-11-20.md. The linked restore stack was itself compiled with -DHV_STOP_AFTER_RESTORE_OPTIONS
# (tools/restore-selftest-build.sh, used unchanged, alongside the existing stop A/B/C/D flags for G0/G3/G4/G5): a second, independent stop baked into the
# vendored tool itself, in case FLAG_STOP_AFTER_RESTORE_OPTIONS ever failed to apply. A diagnostic program: it is a separate binary, and -DHV_INSTALL_G6
# is set nowhere else (tools/check-no-test-flags.sh). Named vmrun_g6 on purpose: it is not an installer.
# Run after tools/engine-build.sh, tools/devices-build.sh and tools/restore-selftest-build.sh with the same compiler; <output dir> is the engine's, <restore dir> the
# selftest's. Usage: CC=<compiler> [EXTRA_LIBS=...] [PF_THREADLIB="" on Android: bionic has no libpthread] tools/vmrun-install-g6-build.sh <output dir> <restore build dir>
set -u
OUT=${1:?output dir}
RS=${2:?restore build dir}
: "${CC:?CC}"
TP=third_party
LIB="$RS/rs/lib.a"; ZIP="$RS/rs/libzip-build/lib/libzip.a"
[ -f "$LIB" ] && [ -f "$ZIP" ] || { echo "FAILED  the restore stack is not built in $RS (tools/restore-selftest-build.sh)"; exit 1; }
FL="-std=gnu11 -D_GNU_SOURCE -O2 -g -w -DHAVE_CONFIG_H -Icore -Ihost -Idevices -I$TP/restore_config -I$TP -I$TP/libtatsu/include -I$TP/libusb-1.0 -I$TP/curl_include -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libirecovery/include -I$TP/idevicerestore/src -I$RS/rs/libzip-build -I$TP/libzip/lib -DHV_STOP_AFTER_RESTORE_OPTIONS -DHV_INSTALL_G6"
R="$OUT/installg6"
mkdir -p "$R"
for f in install install_g6; do
  $CC $FL -c host/$f.c -o "$R/$f.o" 2> "$R/$f.log" || { echo "FAILED  host/$f.c"; head -20 "$R/$f.log"; exit 1; }
done
$CC $FL -c host/vmrun.c -o "$R/vmrun.o" 2> "$R/vmrun.o.log" || { echo "FAILED  host/vmrun.c"; head -20 "$R/vmrun.o.log"; exit 1; }
# the disk object and vm.o of the engine build are reused: they were built with the same flags minus the switch, which neither of them reads
eobjs=""; for o in classify operands translate dispatch mmu memop exception sysreg machine jit_region disk enter; do eobjs="$eobjs $OUT/$o.o"; done
dobjs="$(ls $OUT/dev_*.o)"
vobjs="$OUT/vm_vm.o $OUT/vm_stubs_portable.o $OUT/vm_usb_host.o"
if $CC $FL "$R/vmrun.o" "$R/install.o" "$R/install_g6.o" $vobjs $eobjs $dobjs "$LIB" "$ZIP" ${EXTRA_LDFLAGS:-} ${EXTRA_LIBS:-} -lz -lm ${PF_THREADLIB--lpthread} -o "$OUT/vmrun_g6" 2> "$R/link.log"; then
  echo "linked  $OUT/vmrun_g6"
else
  echo "FAILED  link vmrun_g6"; grep -o "undefined reference to .*" "$R/link.log" | sort -u | head -30; head -30 "$R/link.log" | cut -c1-200; exit 1
fi
