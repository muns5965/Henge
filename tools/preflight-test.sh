#!/bin/bash
# host/restore_preflight.c: the real restore tool against the stand-in ROM (tests/test_irecv.c) and a fake IPSW. Linux. Needs the restore stack: it builds it with
# tools/restore-selftest-build.sh into <build dir> unless <build dir>/rs/lib.a exists already.
# Usage: [CC=cc] tools/preflight-test.sh <build dir>
set -u
OUT=${1:?build dir}
CC=${CC:-cc}
TP=third_party
[ -f "$OUT/rs/lib.a" ] || tools/restore-selftest-build.sh "$OUT" > "$OUT.build.log" 2>&1 || { echo "FAILED  restore stack build"; tail -20 "$OUT.build.log"; exit 1; }
R="$OUT/rs"
FL="-std=gnu11 -D_GNU_SOURCE -O1 -g -w -DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/libusb-1.0 -I$TP/curl_include -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libirecovery/include -I$TP/idevicerestore/src -I$R/libzip-build -I$TP/libzip/lib -Ihost -Idevices -Icore"
mkdir -p "$OUT/pf"
$CC $FL -c host/irecv_probe.c -o "$OUT/pf/irecv_probe.o" || exit 1
$CC $FL -c host/restore_preflight.c -o "$OUT/pf/pf.o" || exit 1
$CC $FL -DHV_PREFLIGHT_NEGATIVE_CONTROL -c host/restore_preflight.c -o "$OUT/pf/pf_neg.o" || exit 1
$CC $FL -c tests/test_preflight.c -o "$OUT/pf/test.o" || exit 1
for v in pf pf_neg; do
  $CC "$OUT/pf/test.o" "$OUT/pf/$v.o" "$OUT/pf/irecv_probe.o" "$R/lib.a" "$R/libzip-build/lib/libzip.a" -lz -lm -lpthread -o "$OUT/pf/test_$v" 2> "$OUT/pf/link_$v.log" || { echo "FAILED link $v"; head -20 "$OUT/pf/link_$v.log"; exit 1; }
done
# the fake IPSW: a ZIP with one unrelated file, so no BuildManifest.plist
python3 - "$OUT/pf/fake.ipsw" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_STORED) as z: z.writestr("README.txt", "not an IPSW\n")
PY
rc=0
run() { # name binary scenario want... (patterns that must appear in the output)
  local name=$1 bin=$2 sc=$3; shift 3
  rm -rf "$OUT/pf/work"; mkdir -p "$OUT/pf/work"
  timeout 120 "$OUT/pf/$bin" $sc "$OUT/pf/fake.ipsw" "$OUT/pf/work" > "$OUT/pf/$name.out" 2> "$OUT/pf/$name.err"; local r=$?
  echo "=== $name (exit $r)"; grep -E "^(restore preflight|scenario|PASS|FAIL)" "$OUT/pf/$name.out" | cut -c1-230
  # the device's ECID (decimal and hex) must be nowhere in what the program printed
  if grep -qi "3254569351603004626\|2d2a8d94f32ba4d2" "$OUT/pf/$name.out" "$OUT/pf/$name.err"; then echo "ECID PRINTED in $name"; rc=1; fi
  for w in "$@"; do grep -Eq -- "$w" "$OUT/pf/$name.out" || { echo "MISSING in $name: $w"; rc=1; }; done
  return $r
}
run progress test_pf progress "^PASS progress verdict DETECT" || rc=1
run ok test_pf ok "ipsw_open +ok" "init +ok" "progress +ok" "network_requests +ok" "data_transfers +ok" "stopped=noaction +inferred" "usb_handles +ok" "event_subscription +ok" "ipsw_fd +leaked" "1 while open, 1 after" "RESULT: preflight_completed" "usbmuxd +info" || rc=1
run no-ipsw test_pf no-ipsw "ipsw_open +FAILED" "RESULT: FAILED" || { [ $? -ge 100 ] && rc=1; }
run curl test_pf curl "network_requests +FAILED" "([1-9][0-9]*) attempt\(s\), \\1 refused" "server saw 0 connection" "RESULT: FAILED" || { [ $? -ge 100 ] && rc=1; }
run dnload test_pf dnload "data_transfers +FAILED" "RESULT: FAILED" || { [ $? -ge 100 ] && rc=1; }
run negative test_pf_neg ok "init +FAILED" "stopped=noaction +FAILED" "RESULT: FAILED" ; [ $? = 102 ] || { echo "negative control: expected exit 102 (the ok scenario's expectation not met)"; rc=1; }
# the files the run left in its work dir: nothing beyond usbmuxd's own (no rootfs, no manifest)
echo "work dir after the ok run:"; ls -la "$OUT/pf/work" | tail -n +2
exit $rc
