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
FL="-std=gnu11 -D_GNU_SOURCE -O1 -g -w -DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/libusb-1.0 -I$TP/curl_include -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libirecovery/include -I$TP/idevicerestore/src -I$R/libzip-build -I$TP/libzip/lib -DHV_STOP_BEFORE_ROOTFS -Ihost -Idevices -Icore"
mkdir -p "$OUT/pf"
$CC $FL -c host/irecv_probe.c -o "$OUT/pf/irecv_probe.o" || exit 1
$CC $FL -c host/restore_preflight.c -o "$OUT/pf/pf.o" || exit 1
$CC $FL -DHV_PREFLIGHT_NEGATIVE_CONTROL -c host/restore_preflight.c -o "$OUT/pf/pf_neg.o" || exit 1
$CC $FL -c tests/test_preflight.c -o "$OUT/pf/test.o" || exit 1
for v in pf pf_neg; do
  $CC "$OUT/pf/test.o" "$OUT/pf/$v.o" "$OUT/pf/irecv_probe.o" "$R/lib.a" "$R/libzip-build/lib/libzip.a" -lz -lm -lpthread -o "$OUT/pf/test_$v" 2> "$OUT/pf/link_$v.log" || { echo "FAILED link $v"; head -20 "$OUT/pf/link_$v.log"; exit 1; }
done
# fake IPSWs: ZIPs with and without what stop A looks at. fake.ipsw has no BuildManifest (stage 0). The a_* ones have a BuildManifest.plist with an erase identity and a macOS
# recovery identity for vma2macosap (the model the stand-in ROM reports), and the components they name; the root filesystem is stored (a_stored) or deflated (a_deflated).
python3 - "$OUT/pf" <<'PY'
import sys, zipfile, plistlib, os
d = sys.argv[1]
with zipfile.ZipFile(os.path.join(d, "fake.ipsw"), "w", zipfile.ZIP_STORED) as z: z.writestr("README.txt", "not an IPSW\n")
def manifest(devclass="vma2macosap", extra_missing=False):
    comps = {"OS": {"Info": {"Path": "rootfs.dmg"}}, "iBSS": {"Info": {"Path": "Firmware/dfu/iBSS.fake.dfu"}}, "iBEC": {"Info": {"Path": "Firmware/dfu/iBEC.fake.dfu"}}}
    if extra_missing: comps["KernelCache"] = {"Info": {"Path": "kernelcache.not-in-the-zip"}}
    erase = {"ApChipID": "0xFE00", "ApBoardID": "0x20", "Info": {"DeviceClass": devclass, "Variant": "Customer Erase Install (IPSW)", "RestoreBehavior": "Erase"}, "Manifest": comps}
    rec = {"ApChipID": "0xFE00", "ApBoardID": "0x20", "Info": {"DeviceClass": devclass, "Variant": "macOS Customer", "MacOSVariant": "macOS Customer"}, "Manifest": {"OS": {"Info": {"Path": "rootfs.dmg"}}}}
    return plistlib.dumps({"ProductVersion": "15.6.1", "ProductBuildVersion": "24G90", "SupportedProductTypes": ["VirtualMac2,1"], "BuildIdentities": [erase, rec]})
def make(name, method, **kw):
    with zipfile.ZipFile(os.path.join(d, name), "w") as z:
        z.writestr("BuildManifest.plist", manifest(**kw), zipfile.ZIP_STORED)
        z.writestr("Firmware/dfu/iBSS.fake.dfu", b"ibss", zipfile.ZIP_STORED)
        z.writestr("Firmware/dfu/iBEC.fake.dfu", b"ibec", zipfile.ZIP_STORED)
        z.writestr("rootfs.dmg", (b"0123456789abcdef" * 16384), method)   # 256 KiB
make("a_stored.ipsw", zipfile.ZIP_STORED)
make("a_deflated.ipsw", zipfile.ZIP_DEFLATED)
make("a_missing.ipsw", zipfile.ZIP_STORED, extra_missing=True)
make("a_wrongmodel.ipsw", zipfile.ZIP_STORED, devclass="j999ap")
PY
rc=0
run() { # name binary scenario ipsw want... (patterns that must appear in the output)
  local name=$1 bin=$2 sc=$3 ip=$4; shift 4
  rm -rf "$OUT/pf/work"; mkdir -p "$OUT/pf/work"
  timeout 120 "$OUT/pf/$bin" $sc "$OUT/pf/$ip" "$OUT/pf/work" > "$OUT/pf/$name.out" 2> "$OUT/pf/$name.err"; local r=$?
  echo "=== $name (exit $r)"; grep -E "^(restore preflight|scenario|PASS|FAIL)" "$OUT/pf/$name.out" | cut -c1-230
  # the device's ECID (decimal and hex) must be nowhere in what the program printed
  if grep -qi "3254569351603004626\\|2d2a8d94f32ba4d2" "$OUT/pf/$name.out" "$OUT/pf/$name.err"; then echo "ECID PRINTED in $name"; rc=1; fi
  for w in "$@"; do grep -Eq -- "$w" "$OUT/pf/$name.out" || { echo "MISSING in $name: $w"; rc=1; }; done
  return $r
}
run progress test_pf progress fake.ipsw "^PASS progress verdict DETECT" "^PASS progress verdict \\(stop A\\) PREPARE 0.0" || rc=1
run ok test_pf ok fake.ipsw "ipsw_open +ok" "init +ok" "progress +ok" "network_requests +ok" "data_transfers +ok" "stopped=noaction +inferred" "usb_handles +ok" "event_subscription +ok" "net_guard +ok" "connections +ok" "ipsw_fd +ok" "RESULT: preflight_completed stopped=noaction" "usbmuxd +ok.*returned and was joined" || rc=1
run no-ipsw test_pf no-ipsw fake.ipsw "ipsw_open +FAILED" "RESULT: FAILED" || { [ $? -ge 100 ] && rc=1; }
run curl test_pf curl fake.ipsw "network_requests +FAILED" "([1-9][0-9]*) attempt\\(s\\), \\1 refused" "server saw 0 connection" "RESULT: FAILED" || { [ $? -ge 100 ] && rc=1; }
run dnload test_pf dnload fake.ipsw "data_transfers +FAILED" "RESULT: FAILED" || { [ $? -ge 100 ] && rc=1; }
run negative test_pf_neg ok fake.ipsw "init +FAILED" "stopped=noaction +FAILED" "RESULT: FAILED" ; [ $? = 102 ] || { echo "negative control: expected exit 102 (the ok scenario's expectation not met)"; rc=1; }
run hang test_pf hang fake.ipsw "init +FAILED" "TIMEOUT" "RESULT: FAILED" || { [ $? -ge 100 ] && rc=1; }
# stop A
run a-stored test_pf a-ok a_stored.ipsw "init +ok" "progress +ok" "stopped=before_rootfs +direct" "identity +ok.*Customer Erase Install" "macos_variant +info +present" "components +ok" "rootfs_entry +ok.*method stored.*seekable.*agrees" \
  "extraction_rule +info.*extract no" "local_writes +info.*0 new file" "network_requests +ok" "connections +ok" "data_transfers +ok" "ipsw_fd +ok" "usbmuxd +ok" "threads +ok" "RESULT: preflight_completed stopped=before_rootfs \\(observed\\)" || rc=1
run a-deflated test_pf a-ok a_deflated.ipsw "stopped=before_rootfs +direct" "rootfs_entry +ok.*method deflate.*NOT seekable.*agrees" "extraction_rule +info.*extract YES.*This run extracted nothing" "local_writes +info.*0 new file" "RESULT: preflight_completed stopped=before_rootfs" || rc=1
run a-missing test_pf a-bad-component a_missing.ipsw "init +FAILED" "stopped=before_rootfs +FAILED" "identity +FAILED" "RESULT: FAILED" || { [ $? -ge 100 ] && rc=1; }
run a-wrongmodel test_pf a-bad-identity a_wrongmodel.ipsw "init +FAILED" "stopped=before_rootfs +FAILED" "RESULT: FAILED" || { [ $? -ge 100 ] && rc=1; }
run a-curl test_pf a-curl a_stored.ipsw "network_requests +FAILED" "server saw 0 connection" "RESULT: FAILED" || { [ $? -ge 100 ] && rc=1; }
run a-dnload test_pf a-dnload a_stored.ipsw "data_transfers +FAILED" "RESULT: FAILED" || { [ $? -ge 100 ] && rc=1; }
# negative control for stop A: without the stop the tool goes on and extracts the deflated rootfs into the work directory (the fake manifest then stops it before any signing request); local_writes must show the file
run a-negative test_pf_neg a-ok a_deflated.ipsw "init +FAILED" "stopped=before_rootfs +FAILED" "local_writes +[A-Za-z]+ +files in the work directory: 0 before, 1 after; 1 new file.*rootfs.dmg" "RESULT: FAILED"; r=$?
[ $r -ge 100 ] || { echo "a-negative: expected the ok scenario's expectation not to be met (exit >= 100), got $r"; rc=1; }
# the files the run left in its work dir: nothing beyond usbmuxd's own (no rootfs, no manifest)
echo "work dir after the ok run:"; ls -la "$OUT/pf/work" | tail -n +2
exit $rc
