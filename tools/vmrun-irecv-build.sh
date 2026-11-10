#!/bin/bash
# vmrun with libirecovery in it (--irecv-probe): host/vmrun.c -DHV_IRECV_PROBE + host/irecv_probe.c + host/libusb_shim.c + the vendored libirecovery and the two
# glue files it needs. Run after tools/engine-build.sh and tools/devices-build.sh with the same compiler and output dir (it reuses their objects).
# A separate program from vmrun on purpose: if this one does not build, the plain vmrun and everything else still ship.
# Usage: CC=<compiler> [EXTRA_LIBS=...] tools/vmrun-irecv-build.sh <output dir>
set -u
OUT=${1:?output dir}
: "${CC:?CC}"
TP=third_party
FL="-std=gnu11 -D_GNU_SOURCE -O2 -g -w -DHAVE_CONFIG_H -Icore -Ihost -Idevices -I$TP/restore_config -I$TP -I$TP/libusb-1.0 -I$TP/libirecovery/include -I$TP/libimobiledevice-glue/include -I$TP/libplist/include"
R="$OUT/irecv"
mkdir -p "$R"
objs=""
for f in $TP/libimobiledevice-glue/src/collection.c $TP/libimobiledevice-glue/src/thread.c $TP/libirecovery/src/libirecovery.c host/libusb_shim.c host/irecv_probe.c; do
  o="$R/$(basename "${f%.*}").o"
  extra=""; [ "$(basename "$f")" = libirecovery.c ] && extra='-DPACKAGE_VERSION="1.3.1-henge"'
  $CC $FL $extra -c "$f" -o "$o" 2> "$o.log" || { echo "FAILED  $f"; head -20 "$o.log"; exit 1; }
  objs="$objs $o"
done
$CC $FL -DHV_IRECV_PROBE -c host/vmrun.c -o "$R/vmrun.o" 2> "$R/vmrun.o.log" || { echo "FAILED  host/vmrun.c"; head -20 "$R/vmrun.o.log"; exit 1; }
eobjs=""; for o in classify operands translate dispatch mmu memop exception sysreg machine jit_region disk enter; do eobjs="$eobjs $OUT/$o.o"; done
dobjs="$(ls $OUT/dev_*.o)"
vobjs="$OUT/vm_vm.o $OUT/vm_stubs_portable.o $OUT/vm_usb_host.o"
if $CC $FL "$R/vmrun.o" $objs $vobjs $eobjs $dobjs ${EXTRA_LDFLAGS:-} ${EXTRA_LIBS:-} -o "$OUT/vmrun_irecv" 2> "$R/link.log"; then
  echo "linked  $OUT/vmrun_irecv"
else
  echo "FAILED  link vmrun_irecv"; head -30 "$R/link.log"; exit 1
fi
