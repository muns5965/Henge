#!/bin/bash
# libirecovery (the vendored source) + host/libusb_shim.c + host/irecv_probe.c against a stand-in for the ROM's USB side. Linux, any compiler.
# Usage: [CC=cc] tools/irecv-test.sh <build dir>
set -u
OUT=${1:?build dir}
CC=${CC:-cc}
TP=third_party
mkdir -p "$OUT"
FL="-std=gnu11 -D_GNU_SOURCE -O1 -g -w -Werror=implicit-function-declaration -DHAVE_CONFIG_H -Ihost -Idevices -Icore -I$TP/restore_config -I$TP -I$TP/libusb-1.0 -I$TP/libirecovery/include -I$TP/libimobiledevice-glue/include -I$TP/libplist/include"
objs=""
for f in $TP/libimobiledevice-glue/src/collection.c $TP/libimobiledevice-glue/src/thread.c $TP/libirecovery/src/libirecovery.c host/libusb_shim.c host/irecv_probe.c devices/virtio_usb.c host/usb_host.c; do
  o="$OUT/$(basename "${f%.*}").o"
  extra=""; [ "$(basename "$f")" = libirecovery.c ] && extra='-DPACKAGE_VERSION="1.3.1-henge"'
  $CC $FL $extra -c "$f" -o "$o" 2> "$o.log" || { echo "FAILED  $f"; head -20 "$o.log"; exit 1; }
  objs="$objs $o"
done
$CC $FL tests/test_irecv.c $objs -lpthread -o "$OUT/test_irecv" 2> "$OUT/link.log" || { echo "FAILED  link"; head -30 "$OUT/link.log"; exit 1; }
rc=0
for sc in ok configured stall-setcfg wrong-ecid no-device reattach gone inject-dnload inject-bulk; do
  echo "=== scenario $sc"
  HV_LIBUSB_LOG=1 timeout 60 "$OUT/test_irecv" $sc > "$OUT/$sc.out" 2> "$OUT/$sc.err"; r=$?
  grep -E "^(irecv probe|scenario)" "$OUT/$sc.out" | cut -c1-260
  if [ $r != 0 ]; then echo "SCENARIO $sc FAILED (exit $r)"; tail -20 "$OUT/$sc.err"; rc=1; else echo "PASS $sc"; fi
  # what the result table must say (the exit status alone cannot tell which stage failed)
  case $sc in
    ok|configured|reattach) want="events:ok boundary:ok" ;;
    gone) want="events:FAILED boundary:ok open:ok" ;;
    inject-dnload|inject-bulk) want="events:ok boundary:FAILED open:ok" ;;
    wrong-ecid) want="events:FAILED boundary:ok" ;;
    stall-setcfg) want="events:ok boundary:ok open:FAILED" ;;
    *) want="" ;;
  esac
  for w in $want; do
    if ! grep -Eq "^irecv probe: ${w%%:*} +${w##*:}( |$)" "$OUT/$sc.out"; then echo "SCENARIO $sc: expected '${w%%:*} ${w##*:}' in the result table"; rc=1; fi
  done
done
exit $rc
