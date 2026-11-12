#!/bin/sh
# Guards the shipped Android build against the test-only switches, and requires its HTTPS-only switch.
#   HV_TSS_ALLOW_HTTP_TEST  admits http://127.0.0.1 / localhost in libtatsu   (tools/tatsu-test.sh only)
#   HV_RZ_ALLOW_HTTP_TEST   same for host/remote_zip.c                          (tools/remote-zip-test.sh only; the object check is skipped: the shipped
#                           builds do not contain remote_zip.c yet, the source scan below still covers them)
#   HV_DL_ALLOW_HTTP_TEST   same for host/ipsw_dl.c; HV_DL_TEST: the test arguments of tests/ipsw_dl_tool.c (tools/ipswdl-test.sh only)
#   HV_PREFLIGHT_NEGATIVE_CONTROL  drops FLAG_NOACTION from host/restore_preflight.c   (tools/preflight-test.sh only)
#   HV_PREFLIGHT_TEST_HOLD_ASYNC  holds one async task in host/restore_preflight.c to inject a client_release failure   (tools/preflight-test.sh only)
#   HV_ALLOW_CA_ENV         lets HV_EXTRA_ANCHOR / HV_CA_PATH widen trust      (the test builds of the HTTP client only)
#   HV_TEST_HOOKS           compiles the observation-only test hooks of host/restore_registry.c (docs/fake-device-s4-test-hook-design-2026-11-11.md §3); test builds only
#   --wrap=thread_new / tests/fake_device/wrap_thread.c   the test-only link-time wrapper of tools/fake-restore-test.sh; never in a shipped link
#   HV_INSTALL_G0           the G0 test of the install entry point (host/install_g0.h): host/install.c and host/vmrun.c compile its hooks only with it; set only by tools/vmrun-install-g0-build.sh (the vmrun_g0 test program) and tools/install-g0-test.sh
#   HV_INSTALL_G1           the G1 test of the install entry point and the restore tool's signing path (host/install_g1.h): host/install.c compiles its hooks only with it; set only by tools/install-g1-test.sh
#   FLAG_STOP_BEFORE_ROOTFS  (third_party/idevicerestore/src/hv_stop.h) is compiled into the Android restore stack but only host/restore_preflight.c ever sets it
# 1. source level: no build script or workflow that makes the shipped binaries sets them; the shipped libtatsu build sets HV_TSS_HTTPS_ONLY.
# 2. object level: compiles libtatsu's tss.c and host/curl_mbedtls.c the way the shipped build does and looks for the test strings in the objects.
# Usage: [CC=..] tools/check-no-test-flags.sh [workdir]
set -e
cd "$(dirname "$0")/.."
CC=${CC:-cc}; W=${1:-build-flagcheck}; mkdir -p "$W"
fail=0
bad() { echo "FAIL $*"; fail=1; }
# files that build what ships (the test scripts tatsu-test.sh, curl-test.sh, download-test.sh and the engine workflow's test steps are the places allowed to use them)
for f in tools/restore-selftest-build.sh tools/bmprobe-build.sh tools/ipswdl-build.sh tools/restore-android-check.sh tools/build_ios_restore.sh tools/engine-build.sh tools/devices-build.sh tools/vmrun-irecv-build.sh tools/vmrun-preflight-build.sh tools/vmrun-install-g0-build.sh \
         android/app/src/main/cpp/CMakeLists.txt android/app/build.gradle .github/workflows/android-apk.yml .github/workflows/build-henge.yml ios/project.yml Makefile; do
  [ -f "$f" ] || continue
  if grep -n "HV_TSS_ALLOW_HTTP_TEST\|HV_RZ_ALLOW_HTTP_TEST\|HV_DL_ALLOW_HTTP_TEST\|HV_DL_TEST\|HV_PREFLIGHT_NEGATIVE_CONTROL\|HV_PREFLIGHT_TEST_HOLD_ASYNC\|HV_TEST_HOOKS\|wrap=thread_new\|wrap_thread\.c" "$f" | grep -v '^[0-9]*:[[:space:]]*#' > /dev/null; then bad "$f sets a test-only switch (HV_TSS_/HV_RZ_/HV_DL_ALLOW_HTTP_TEST, HV_DL_TEST, HV_TEST_HOOKS, thread_new wrapper)"; fi
  # the workflow comment that says "No -DHV_ALLOW_CA_ENV here" is prose; a real setting is -DHV_ALLOW_CA_ENV or CURLTOOL_CFLAGS=...HV_ALLOW_CA_ENV
  if grep -n "\-DHV_ALLOW_CA_ENV\|CURLTOOL_CFLAGS=[^ ]*HV_ALLOW_CA_ENV" "$f" | grep -v 'No -DHV_ALLOW_CA_ENV' > /dev/null; then bad "$f sets HV_ALLOW_CA_ENV"; fi
done
for f in host/install.c host/ipsw_dl.c host/tss_probe.c; do
  [ -f "$f" ] && grep -q "FLAG_STOP_BEFORE_ROOTFS" "$f" && bad "$f sets the stop-A test flag"
done
# the G0 switch: only its own build script and its Linux test may set it (never the app's CMake, gradle, workflows for the main program, the iOS project, the Makefile)
if grep -rln "DHV_INSTALL_G0" tools android .github ios Makefile 2>/dev/null | grep -v "^tools/vmrun-install-g0-build.sh$\|^tools/install-g0-test.sh$\|^tools/check-no-test-flags.sh$" > /dev/null; then bad "something other than tools/vmrun-install-g0-build.sh and tools/install-g0-test.sh sets -DHV_INSTALL_G0"; fi
# only host/install_g0.c sets the stop flag for the install entry point; host/install.c keeps the plain flag-free source (checked above) and reaches it through hv_g0_extra_flags
grep -q "hv_g0_extra_flags" host/install.c && ! grep -q "FLAG_STOP_BEFORE_ROOTFS" host/install.c || bad "host/install.c does not go through the G0 hook, or names the stop flag"
# the G1 switch: only its Linux test may set it
if grep -rln "DHV_INSTALL_G1" tools android .github ios Makefile 2>/dev/null | grep -v "^tools/install-g1-test.sh$\|^tools/check-no-test-flags.sh$" > /dev/null; then bad "something other than tools/install-g1-test.sh sets -DHV_INSTALL_G1"; fi
grep -q "hv_g1_extra_flags" host/install.c && ! grep -q "FLAG_SHSHONLY" host/install.c || bad "host/install.c does not go through the G1 hook, or names the signing-only flag"
# the G1 test program is not part of any shipped source list
if grep -rn "install_g1" android/app/src android/CMakeLists.txt CMakeLists.txt 2>/dev/null | grep . > /dev/null; then bad "a shipped build mentions host/install_g1"; fi
# the install disk is created or reset only by an explicit, separately approved step: no source of the app calls those functions yet (tests excluded)
if grep -rn "hv_install_disk_create\|hv_install_disk_reset" host devices core android/app/src --include=*.c --include=*.h --include=*.m --include=*.java --include=*.cpp 2>/dev/null | grep -v "^host/disk_policy\.[ch]:" > /dev/null; then bad "something calls hv_install_disk_create / hv_install_disk_reset"; fi
for f in tools/restore-selftest-build.sh tools/restore-android-check.sh; do
  grep -q "HV_TSS_HTTPS_ONLY" "$f" || bad "$f builds libtatsu without HV_TSS_HTTPS_ONLY"
done
# the restore tool is started through host/restore_session.c only (it refuses to run without the connection policy), and nothing else installs or removes the policy
if grep -rnE "idevicerestore_start[[:space:]]*\(" host devices core android/app/src --include=*.c --include=*.h --include=*.m --include=*.cpp 2>/dev/null | grep -v "^host/restore_session\.[ch]:" | grep -v "^host/install\.c:" > /dev/null; then bad "idevicerestore_start is called outside host/restore_session.c"; fi
# host/install.c still starts the tool directly in its non-Android branch (iOS, Linux); its Android branch must go through the session
grep -q "hv_restore_session_run_client" host/install.c || bad "host/install.c does not start the tool through the restore session on Android"
if grep -rn "hv_net_guard_install\|hv_net_guard_uninstall" host devices core android/app/src --include=*.c --include=*.m --include=*.cpp 2>/dev/null | grep -v "^host/net_guard\.c:" | grep -v "^host/restore_session\.c:" > /dev/null; then bad "the connection policy is installed or removed outside host/restore_session.c"; fi
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
# the test-only loopback allowance in the connection policy: absent from the shipped object, present in the test flavour (control), never set by a shipped build script
$CC -O1 -w -std=gnu11 -D_GNU_SOURCE -Ihost -I$TP/libimobiledevice-glue/include -c host/net_guard.c -o "$W/ng_ship.o"
if nm "$W/ng_ship.o" | grep -q "hv_net_guard_test_allow_loopback" || strings "$W/ng_ship.o" | grep -q "test loopback"; then bad "shipped net_guard.o contains the test loopback allowance"; fi
$CC -O1 -w -std=gnu11 -D_GNU_SOURCE -DHV_NET_TEST_LOOPBACK -Ihost -I$TP/libimobiledevice-glue/include -c host/net_guard.c -o "$W/ng_test.o"
nm "$W/ng_test.o" | grep -q "hv_net_guard_test_allow_loopback" || bad "control: the test flavour of net_guard.o lacks the loopback allowance (check is vacuous)"
if grep -rln "HV_NET_TEST_LOOPBACK" android CMakeLists.txt tools/restore-android-check.sh tools/restore-selftest-build.sh 2>/dev/null | grep . > /dev/null; then bad "a shipped build file mentions HV_NET_TEST_LOOPBACK"; fi
if grep -rn "hv_net_guard_test_allow_loopback" host devices core android/app/src --include=*.c --include=*.m --include=*.cpp 2>/dev/null | grep -v "^host/net_guard\.c:\|^host/install_g1\.c:" > /dev/null; then bad "app code calls the test loopback function"; fi
# the registry's observation-only test hooks (HV_TEST_HOOKS): absent from the shipped object, present in the test flavour (control), never called by app code
if [ -f host/restore_registry.c ]; then
  $CC -O1 -w -std=gnu11 -D_GNU_SOURCE -Ihost -c host/restore_registry.c -o "$W/reg_ship.o"
  if nm "$W/reg_ship.o" | grep -q "hv_test_hook" || strings "$W/reg_ship.o" | grep -q "hv_test_hook"; then bad "shipped restore_registry.o contains the test hooks"; fi
  $CC -O1 -w -std=gnu11 -D_GNU_SOURCE -DHV_TEST_HOOKS -Ihost -c host/restore_registry.c -o "$W/reg_test.o"
  nm "$W/reg_test.o" | grep -q "hv_test_hook" || bad "control: the test flavour of restore_registry.o lacks the hooks (check is vacuous)"
  if grep -rn "hv_test_hook" host devices core android/app/src --include=*.c --include=*.h --include=*.m --include=*.cpp 2>/dev/null | grep -v "^host/restore_registry\.[ch]:" > /dev/null; then bad "app code refers to the registry test hooks"; fi
else
  echo "note: host/restore_registry.c not present; the HV_TEST_HOOKS object check is skipped"
fi
if grep -rln "HV_TEST_HOOKS" android CMakeLists.txt tools/restore-android-check.sh tools/restore-selftest-build.sh 2>/dev/null | grep . > /dev/null; then bad "a shipped build file mentions HV_TEST_HOOKS"; fi
[ $fail = 0 ] && echo "OK  shipped build: no test switches, https-only libtatsu"
exit $fail
