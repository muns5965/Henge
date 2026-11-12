#!/bin/bash
# G1: tests/test_install_g1.c + host/install_g1.c + host/install.c (-D__ANDROID__ -DHV_INSTALL_G1) with the real session, connection policy and restore tool, the stand-in
# ROM (tests/test_irecv.c), a fake macOS IPSW and the synthetic TLS signing server tests/g1_tss_server.py on 127.0.0.1. Linux. No real signing server is contacted.
# Needs the restore stack (built when missing), python3 and openssl.
# Usage: [CC=cc] [CASES="ok refuse1 ..."] tools/install-g1-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}; TP=third_party; R="$OUT/rs"
[ -f "$R/lib.a" ] || tools/restore-selftest-build.sh "$OUT" > "$OUT.build.log" 2>&1 || { echo "FAILED restore stack build"; exit 1; }
G="$OUT/g1"; mkdir -p "$G"
INC="-DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/libusb-1.0 -I$TP/curl_include -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libusbmuxd/include -I$TP/libimobiledevice/include -I$TP/libirecovery/include -I$R/libzip-build -I$TP/libzip/lib -I$TP/libtatsu/include -I$TP/idevicerestore/src -Ihost -Idevices -Icore"
FLG="-std=gnu11 -D_GNU_SOURCE -O1 -g -w $INC -D__ANDROID__ -DHV_INSTALL_G1 -DHV_STOP_BEFORE_ROOTFS"
# the HTTP client and the connection policy in their TEST flavour (the shipped ones read no test setting): HV_CA_PATH for the test CA, one approved loopback port
$CC $FLG -DHV_ALLOW_CA_ENV -c host/curl_mbedtls.c -o "$G/curl_mbedtls.o" -I$TP/mbedtls/include || { echo "FAILED build curl_mbedtls"; exit 1; }
$CC $FLG -DHV_NET_TEST_LOOPBACK -c host/net_guard.c -o "$G/net_guard.o" || { echo "FAILED build net_guard"; exit 1; }
$CC $FLG -c tests/test_install_g1.c -o "$G/test.o" || { echo "FAILED build test"; exit 1; }
$CC $FLG "$G/test.o" host/install.c host/install_g1.c "$G/curl_mbedtls.o" "$G/net_guard.o" host/irecv_probe.c "$R/lib.a" "$R/libzip-build/lib/libzip.a" -lz -lm -lpthread -o "$G/t" 2> "$G/link.log" \
  || { echo "FAILED link"; grep -o "undefined reference to .*\|multiple definition.*" "$G/link.log" | sort -u | head; head -5 "$G/link.log" | cut -c1-200; exit 1; }
# a fake macOS IPSW: what the tool needs to build its three requests, no real firmware (a few bytes per component)
python3 - "$G" <<'PY'
import sys, zipfile, plistlib, os
d = sys.argv[1]
dig = b"\x11" * 48
def comp(path, extra=None):
    e = {"Digest": dig, "Info": {"Path": path}, "Trusted": True, "EPRO": True, "ESEC": True}
    if extra: e.update(extra)
    return e
erase_m = {"OS": comp("rootfs.dmg"), "iBSS": comp("Firmware/dfu/iBSS.fake.dfu"), "iBEC": comp("Firmware/dfu/iBEC.fake.dfu")}
erase = {"ApChipID": "0xFE00", "ApBoardID": "0x20", "ApSecurityDomain": "0x1", "UniqueBuildID": b"\x22" * 20,
         "Info": {"DeviceClass": "vma2macosap", "Variant": "Customer Erase Install (IPSW)", "RestoreBehavior": "Erase", "RequiresUIDMode": False}, "Manifest": erase_m}
rec = {"ApChipID": "0xFE00", "ApBoardID": "0x20", "ApSecurityDomain": "0x1", "UniqueBuildID": b"\x33" * 20,
       "Info": {"DeviceClass": "vma2macosap", "Variant": "macOS Customer", "MacOSVariant": "macOS Customer"}, "Manifest": {"OS": comp("rootfs.dmg")}}
man = plistlib.dumps({"ProductVersion": "15.6.1", "ProductBuildVersion": "24G90", "SupportedProductTypes": ["VirtualMac2,1"], "BuildIdentities": [erase, rec]})
with zipfile.ZipFile(os.path.join(d, "g1.ipsw"), "w") as z:
    z.writestr("BuildManifest.plist", man, zipfile.ZIP_STORED)
    for n in ("Firmware/dfu/iBSS.fake.dfu", "Firmware/dfu/iBEC.fake.dfu"): z.writestr(n, b"fake", zipfile.ZIP_STORED)
    z.writestr("rootfs.dmg", b"0123456789abcdef" * 4096, zipfile.ZIP_STORED)
PY
# the synthetic server
S="$G/srv"; rm -rf "$S"; mkdir -p "$S"
python3 tests/g1_tss_server.py "$S" > "$G/server.log" 2>&1 &
SPID=$!
trap 'kill $SPID 2>/dev/null' EXIT
for i in $(seq 1 100); do [ -f "$S/ready" ] && break; sleep 0.1; done
[ -f "$S/ready" ] || { echo "FAILED the synthetic server did not start"; cat "$G/server.log"; exit 1; }
PORT=$(cat "$S/port")
# a port nobody listens on, for connfail
CLOSED=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
rc=0
reqs() { grep -c "^$1\$" "$S/requests.log" 2>/dev/null; true; }
total_reqs() { wc -l < "$S/requests.log"; }
run() { # name scenario port ca [want patterns...]  (a "c-" scenario must exit 1; the others 0)
  local name=$1 sc=$2 port=$3 ca=$4; shift 4
  rm -rf "$G/work_$name"; mkdir -p "$G/work_$name"
  : > "$S/requests.log"
  HV_CA_PATH="$S/$ca" HV_VUSB_LOG= timeout 240 "$G/t" $sc "$G/g1.ipsw" "$G/work_$name" $port > "$G/$name.out" 2> "$G/$name.err"; local r=$?
  local want=0; case $sc in c-*) want=1;; esac
  echo "=== $name (exit $r, want $want)"; grep -E "^(install g1|scenario)" "$G/$name.out" | cut -c1-260
  [ $r = $want ] || { echo "UNEXPECTED exit $r in $name"; rc=1; tail -5 "$G/$name.err"; }
  # the ECID (decimal and hex) and the device nonce (hex and the tool's spaced dump) must be nowhere in what the program printed (the control that shows the log)
  if [ $name != c-showlog ]; then
    if grep -qi "3254569351603004626\|2d2a8d94f32ba4d2\|00112233445566778899aabb\|00 11 22 33 44 55" "$G/$name.out" "$G/$name.err"; then echo "ECID OR NONCE PRINTED in $name"; rc=1; fi
  fi
  for w in "$@"; do grep -Eq -- "$w" "$G/$name.out" || { echo "MISSING in $name: $w"; rc=1; }; done
}
CASES=${CASES:-ok refuse1 refuse2 bad connfail timeout badcert redirect external noallow c-noallow-asok c-tickets c-dnload c-injfile c-showlog}
for c in $CASES; do
  case $c in
    ok)       run ok ok $PORT cadir "RESULT: g1_passed" "engine_rc +ok" "tss_requests +ok.*made 3 request" "connections +ok" "device_requests +ok" "local_writes +ok" "ticket_files +ok" "session +ok.*final report 1"
              [ "$(reqs /ok)" = 3 ] || { echo "ok: the server saw $(reqs /ok) request(s), expected 3"; rc=1; } ;;
    refuse1)  run refuse1 refuse1 $PORT cadir "RESULT: g1_passed" "engine_rc +ok" "tss_requests +ok.*made 1 request" "session +ok.*final report 2"
              [ "$(reqs /refuse)" = 1 ] || { echo "refuse1: the server saw $(reqs /refuse) request(s), expected 1"; rc=1; } ;;
    refuse2)  run refuse2 refuse2 $PORT cadir "RESULT: g1_passed" "tss_requests +ok.*made 2 request"
              [ "$(reqs /refuse2)" = 2 ] || { echo "refuse2: the server saw $(reqs /refuse2) request(s), expected 2"; rc=1; } ;;
    bad)      run bad bad $PORT cadir "RESULT: g1_passed" "engine_rc +ok" ;;
    connfail) run connfail connfail $CLOSED cadir "RESULT: g1_passed" "engine_rc +ok"
              [ "$(total_reqs)" = 0 ] || { echo "connfail: the server saw requests"; rc=1; } ;;
    timeout)  run timeout timeout $PORT cadir "RESULT: g1_passed" "engine_rc +ok" ;;
    badcert)  run badcert badcert $PORT cadir_other "RESULT: g1_passed" "tss_requests +ok.*made 1 request"
              [ "$(total_reqs)" = 0 ] || { echo "badcert: the server saw a request over an untrusted certificate"; rc=1; } ;;
    redirect) run redirect redirect $PORT cadir "RESULT: g1_passed" "engine_rc +ok"
              [ "$(reqs /ok)" = 0 ] || { echo "redirect: the redirect target was requested"; rc=1; } ;;
    external) run external external $PORT cadir "RESULT: g1_passed" "connections +ok.*1 refused \\(expected 1 to 1\\)"
              [ "$(total_reqs)" = 0 ] || { echo "external: the server saw requests"; rc=1; } ;;
    noallow)  run noallow noallow $PORT cadir "RESULT: g1_passed" "connections +ok.*1 refused \\(expected 1 to 1\\)"
              [ "$(total_reqs)" = 0 ] || { echo "noallow: the server saw requests although the port was not approved"; rc=1; } ;;
    c-noallow-asok) run c-noallow-asok c-noallow-asok $PORT cadir "RESULT: FAILED" "engine_rc +FAILED" "connections +FAILED" "tss_requests +FAILED" ;;
    c-tickets) run c-tickets c-tickets $PORT cadir "RESULT: FAILED" "ticket_files +FAILED" "local_writes +FAILED.*shsh" ;;
    c-dnload) run c-dnload c-dnload $PORT cadir "RESULT: FAILED" "device_requests +FAILED" ;;
    c-injfile) run c-injfile c-injfile $PORT cadir "RESULT: FAILED" "ticket_files +FAILED" "local_writes +FAILED.*stray.shsh" ;;
    c-showlog) HV_G1_SHOW_LOG=1 run c-showlog ok $PORT cadir "RESULT: g1_passed"
              grep -qi "00 11 22 33 44 55" "$G/c-showlog.out" || { echo "c-showlog: the unfiltered log does not show the nonce, so the filter check proves nothing"; rc=1; } ;;
  esac
done
[ $rc = 0 ] && echo "INSTALL-G1 OK" || echo "INSTALL-G1 FAILED"; exit $rc
