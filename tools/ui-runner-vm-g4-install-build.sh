#!/bin/bash
# host/ui_runner_vm_g4.c: tools/ui-runner-vm-build.sh's VM runner (host/runner_vm_core.c, host/frame_shm.c, host/display_buf.c, the REAL engine) combined
# with tools/vmrun-install-g4-build.sh's G4 diagnostic (host/install.c + host/install_g4.c with -DHV_INSTALL_G4 -DHV_STOP_AFTER_RECOVERYOS_ENTRY, linked
# against the restore stack tools/restore-selftest-build.sh builds) instead of host/vmrun.c. A TEST BUILD ONLY, exactly like vmrun_g4: a separate binary,
# -DHV_INSTALL_G4 is set nowhere else (tools/check-no-test-flags.sh), and nothing shipped builds or packages it. Named ui_runner_vm_g4 on purpose: it is
# not the app's runner (that is host/ui_runner_vm.c, untouched by this script and by this whole diagnostic) and it is not an installer.
#
# Deliberately does NOT run tools/check-runner-vm-safe.sh: that script's job is to confirm host/ui_runner_vm.c's own binary can be shown to keep
# installer=0 / ecid=0 / seed=0 (the safety contract that binary alone upholds). This binary's whole point is the opposite setting (installer=1, a real
# ecid) and running that check against it would correctly FAIL every time -- that is not a defect in this binary, it is a different, separately-approved
# contract (host/ui_runner_vm_g4.c's own g4ui_config_ok, not host/runner_vm_core.c's hv_runner_config_ok). No safety check for THIS contract exists yet;
# none is claimed here.
#
# Run after tools/engine-build.sh, tools/devices-build.sh and tools/restore-selftest-build.sh with the same compiler.
# Usage: CC=<compiler> [EXTRA_LIBS=...] [PF_THREADLIB="" on Android: bionic has no libpthread] tools/ui-runner-vm-g4-install-build.sh <engine dir> <restore build dir> [<output dir>]
set -u
cd "$(dirname "$0")/.."
E=${1:?engine build dir (after tools/engine-build.sh and tools/devices-build.sh)}
RS=${2:?restore build dir (after tools/restore-selftest-build.sh)}
OUT=${3:-$E}
CC=${CC:?CC}
TP=third_party
LIB="$RS/rs/lib.a"; ZIP="$RS/rs/libzip-build/lib/libzip.a"
[ -f "$LIB" ] && [ -f "$ZIP" ] || { echo "FAILED  the restore stack is not built in $RS (tools/restore-selftest-build.sh)"; exit 1; }
mkdir -p "$OUT/uirvmg4"

eobjs=""; for o in classify operands translate dispatch mmu memop exception sysreg machine jit_region disk enter; do eobjs="$eobjs $E/$o.o"; done
dobjs="$(ls $E/dev_*.o)"
vobjs="$E/vm_vm.o $E/vm_stubs_portable.o $E/vm_usb_host.o"
for o in $eobjs $vobjs; do [ -f "$o" ] || { echo "FAILED  missing $o (run the engine and devices builds first)"; exit 1; }; done

# host/ui_runner_vm.c's own three support files: unmodified, compiled plain (no -DHV_INSTALL_G4 -- they do not reference G4 at all).
RCF="-std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -Icore -Ihost -Idevices"
rsrcs="host/runner_vm_core.c host/frame_shm.c host/display_buf.c"
robjs=""
for f in $rsrcs; do
  o="$OUT/uirvmg4/$(basename "$f" .c).o"
  $CC $RCF -c "$f" -o "$o" 2> "$o.log" || { echo "FAILED  $f"; head -30 "$o.log"; exit 1; }
  robjs="$robjs $o"
done

# host/install.c + host/install_g4.c + this binary itself: G4's own flags, same as tools/vmrun-install-g4-build.sh (-w: the vendored restore headers
# this pulls in are not held to this repo's -Wall -Wextra -Werror; host/install.c/install_g4.c/ui_runner_vm_g4.c themselves are, in effect, since none of
# them produce warnings under FL either, but FL is kept identical to the install-g4 script's on purpose so both diagnostics share one definition of
# "the G4 build flags").
FL="-std=gnu11 -D_GNU_SOURCE -O2 -g -w -DHAVE_CONFIG_H -Icore -Ihost -Idevices -I$TP/restore_config -I$TP -I$TP/libtatsu/include -I$TP/libusb-1.0 -I$TP/curl_include -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libirecovery/include -I$TP/idevicerestore/src -I$RS/rs/libzip-build -I$TP/libzip/lib -DHV_STOP_AFTER_RECOVERYOS_ENTRY -DHV_INSTALL_G4"
for f in install install_g4 ui_runner_vm_g4; do
  $CC $FL -c host/$f.c -o "$OUT/uirvmg4/$f.o" 2> "$OUT/uirvmg4/$f.o.log" || { echo "FAILED  host/$f.c"; head -30 "$OUT/uirvmg4/$f.o.log"; exit 1; }
done

if $CC $FL "$OUT/uirvmg4/ui_runner_vm_g4.o" "$OUT/uirvmg4/install.o" "$OUT/uirvmg4/install_g4.o" $robjs $vobjs $eobjs $dobjs "$LIB" "$ZIP" ${EXTRA_LDFLAGS:-} ${EXTRA_LIBS:-} -lz -lm ${PF_THREADLIB--lpthread} -o "$OUT/ui_runner_vm_g4" 2> "$OUT/uirvmg4/link.log"; then
  echo "linked  $OUT/ui_runner_vm_g4"
else
  echo "FAILED  link ui_runner_vm_g4"; grep -o "undefined reference to .*" "$OUT/uirvmg4/link.log" | sort -u | head -30; head -30 "$OUT/uirvmg4/link.log" | cut -c1-200; exit 1
fi
file "$OUT/ui_runner_vm_g4" 2>/dev/null || true
