#!/bin/bash
# STAGE 1: which archive members of the restore stack does the host closure (host/install.c -D__ANDROID__, i.e. idevicerestore_start + usbmuxd_main + everything they reference) pull in?
# A member that is NOT included is not referenced from that closure at object-file granularity. This is evidence about references, not a proof about run-time reachability
# (a member can be included and its thread-creating function never called; function pointers and dlsym are not seen). Linux. Usage: tools/stage1-link-trace.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}; TP=third_party; R="$OUT/rs"
[ -f "$R/lib.a" ] || tools/restore-selftest-build.sh "$OUT" > "$OUT.build.log" 2>&1 || { echo "FAILED restore stack build"; exit 1; }
mkdir -p "$OUT/lt"
INC="-DHAVE_MBEDTLS -I$TP/mbedtls/include -DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libusbmuxd/include -I$TP/libimobiledevice/include -I$TP/libirecovery/include -I$R/libzip-build -I$TP/libzip/lib -I$TP/libtatsu/include -I$TP/idevicerestore/src -I. -Icore -Idevices -Ihost"
$CC -std=gnu11 -D_GNU_SOURCE -O1 -w $INC -D__ANDROID__ tests/stage1_closure_main.c host/install.c tests/fake_device/stubs.c "$R/lib.a" "$R/libzip-build/lib/libzip.a" -lz -lm -lpthread -Wl,-Map="$OUT/lt/map.txt" -o "$OUT/lt/probe" 2> "$OUT/lt/link.log" || { echo "FAILED link"; head -20 "$OUT/lt/link.log"; exit 1; }
members=$(ar t "$R/lib.a" | sort)
inc=$(awk '/^Archive member included/{f=1;next} /^Discarded input sections/{f=0} f' "$OUT/lt/map.txt" | grep -o 'lib\.a(\([^)]*\))' | sed 's/lib\.a(\(.*\))/\1/' | sort -u)
echo "archive members: $(echo "$members" | wc -l), included by the host closure: $(echo "$inc" | wc -l)"
for m in imd_ostrace.o imd_bt_packet_logger.o imd_syslog_relay.o imd_companion_proxy.o imd_installation_proxy.o imd_notification_proxy.o imd_preboard.o muxd_preflight.o imd_reverse_proxy.o idr_fdr.o usbmuxdc_libusbmuxd.o idr_deviceinterfaced.o; do
  if echo "$inc" | grep -qx "$m"; then echo "INCLUDED   $m"; else echo "not included  $m$(echo "$members" | grep -qx "$m" || echo '   (no such member)')"; fi
done
echo "--- why the included service-related members were pulled (the linker's first reference):"
for m in imd_preboard.o muxd_preflight.o imd_reverse_proxy.o usbmuxdc_libusbmuxd.o; do
  awk -v m="$m" '/^Archive member included/{f=1;next} /^Discarded input sections/{f=0} f' "$OUT/lt/map.txt" | grep -A1 "lib\.a($m)" | head -2 | sed -e 's#^[^ ]*/rs/##' -e 's/^ */  /' | paste -sd' '
done
