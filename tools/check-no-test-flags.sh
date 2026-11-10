#!/bin/sh
# Guards the shipped Android build against the test-only switches, and requires its HTTPS-only switch.
#   HV_TSS_ALLOW_HTTP_TEST  admits http://127.0.0.1 / localhost in libtatsu   (tools/tatsu-test.sh only)
#   HV_RZ_ALLOW_HTTP_TEST   same for host/remote_zip.c                          (tools/remote-zip-test.sh only; the object check is skipped: the shipped
#                           builds do not contain remote_zip.c yet, the source scan below still covers them)
#   HV_DL_ALLOW_HTTP_TEST   same for host/ipsw_dl.c; HV_DL_TEST: the test arguments of tests/ipsw_dl_tool.c (tools/ipswdl-test.sh only)
#   HV_PREFLIGHT_NEGATIVE_CONTROL  drops FLAG_NOACTION from host/restore_preflight.c   (tools/preflight-test.sh only)
#   HV_ALLOW_CA_ENV         lets HV_EXTRA_ANCHOR / HV_CA_PATH widen trust      (the test builds of the HTTP client only)
# 1. source level: no build script or workflow that makes the shipped binaries sets them; the shipped libtatsu build sets HV_TSS_HTTPS_ONLY.
# 2. object level: compiles libtatsu's tss.c and host/curl_mbedtls.c the way the shipped build does and looks for the test strings in the objects.
# Usage: [CC=..] tools/check-no-test-flags.sh [workdir]
set -e
cd "$(dirname "$0")/.."
CC=${CC:-cc}; W=${1:-build-flagcheck}; mkdir -p "$W"
fail=0
bad() { echo "FAIL $*"; fail=1; }
# files that build what ships (the test scripts tatsu-test.sh, curl-test.sh, download-test.sh and the engine workflow's test steps are the places allowed to use them)
for f in tools/restore-selftest-build.sh tools/bmprobe-build.sh tools/ipswdl-build.sh tools/restore-android-check.sh tools/build_ios_restore.sh tools/engine-build.sh tools/devices-build.sh tools/vmrun-irecv-build.sh tools/vmrun-preflight-build.sh \
         android/app/src/main/cpp/CMakeLists.txt android/app/build.gradle .github/workflows/android-apk.yml .github/workflows/build-henge.yml ios/project.yml Makefile; do
  [ -f "$f" ] || continue
  if grep -n "HV_TSS_ALLOW_HTTP_TEST\|HV_RZ_ALLOW_HTTP_TEST\|HV_DL_ALLOW_HTTP_TEST\|HV_DL_TEST\|HV_PREFLIGHT_NEGATIVE_CONTROL" "$f" | grep -v '^[0-9]*:[[:space:]]*#' > /dev/null; then bad "$f sets a test-only switch (HV_TSS_/HV_RZ_/HV_DL_ALLOW_HTTP_TEST, HV_DL_TEST)"; fi
  # the workflow comment that says "No -DHV_ALLOW_CA_ENV here" is prose; a real setting is -DHV_ALLOW_CA_ENV or CURLTOOL_CFLAGS=...HV_ALLOW_CA_ENV
  if grep -n "\-DHV_ALLOW_CA_ENV\|CURLTOOL_CFLAGS=[^ ]*HV_ALLOW_CA_ENV" "$f" | grep -v 'No -DHV_ALLOW_CA_ENV' > /dev/null; then bad "$f sets HV_ALLOW_CA_ENV"; fi
done
for f in tools/restore-selftest-build.sh tools/restore-android-check.sh; do
  grep -q "HV_TSS_HTTPS_ONLY" "$f" || bad "$f builds libtatsu without HV_TSS_HTTPS_ONLY"
done
TP=third_party
FL="-O1 -w -DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/curl_include -I$TP/libplist/include -I$TP/libtatsu/include -I$TP/libtatsu/src -I$TP/mbedtls/include -Ihost -DPACKAGE_VERSION=\"1.0.5-henge\""
$CC $FL -DHV_TSS_HTTPS_ONLY -c $TP/libtatsu/src/tss.c -o "$W/tss_ship.o"
$CC -O1 -w -std=gnu11 -D_GNU_SOURCE -Ihost -I$TP/curl_include -I$TP/mbedtls/include -c host/curl_mbedtls.c -o "$W/curl_ship.o"
if strings "$W/tss_ship.o" | grep -q "127.0.0.1"; then bad "shipped tss.o contains the loopback allowance"; fi
if strings "$W/curl_ship.o" | grep -q "HV_EXTRA_ANCHOR"; then bad "shipped curl_mbedtls.o reads HV_EXTRA_ANCHOR"; fi
if ! strings "$W/tss_ship.o" | grep -q "not https"; then bad "shipped tss.o lacks the https check"; fi
$CC -O1 -w -std=gnu11 -D_GNU_SOURCE -Ihost -I$TP/curl_include -I$TP/mbedtls/include -c host/ipsw_dl.c -o "$W/dl_ship.o"
if strings "$W/dl_ship.o" | grep -q "127.0.0.1"; then bad "shipped ipsw_dl.o contains the loopback allowance"; fi
strings "$W/dl_ship.o" | grep -q "not https" || bad "shipped ipsw_dl.o lacks the https check"
$CC -O1 -w -std=gnu11 -D_GNU_SOURCE -DHV_DL_ALLOW_HTTP_TEST -Ihost -I$TP/curl_include -I$TP/mbedtls/include -c host/ipsw_dl.c -o "$W/dl_test.o"
strings "$W/dl_test.o" | grep -q "127.0.0.1" || bad "control: the test flavour of ipsw_dl.o does not contain the loopback allowance (check is vacuous)"
# the test flavour must differ, or the check above proves nothing
$CC $FL -DHV_TSS_HTTPS_ONLY -DHV_TSS_ALLOW_HTTP_TEST -c $TP/libtatsu/src/tss.c -o "$W/tss_test.o"
strings "$W/tss_test.o" | grep -q "127.0.0.1" || bad "control: the test flavour does not contain the loopback allowance (check is vacuous)"
[ $fail = 0 ] && echo "OK  shipped build: no test switches, https-only libtatsu"
exit $fail
