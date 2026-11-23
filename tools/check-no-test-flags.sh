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
#   HV_INSTALL_G2           the G2 diagnostic of the install entry point against Apple's REAL signing server (host/install_g2.h): host/install.c and host/vmrun.c compile its hooks only with it; set only by
#                           tools/vmrun-install-g2-build.sh (the vmrun_g2 program this ships as its own .so, never through the main engine/app CMake build -- see the source-location check below)
#   HV_INSTALL_G3           the G3 diagnostic of the install entry point through TSS and the DFU->Recovery firmware transfer (host/install_g3.h): host/install.c and host/vmrun.c compile its hooks only
#                           with it; set only by tools/vmrun-install-g3-build.sh (the vmrun_g3 program this ships as its own .so, never through the main engine/app CMake build -- see the source-location
#                           check below)
#   HV_INSTALL_G4           the G4 diagnostic of the install entry point through TSS, the DFU->Recovery firmware transfer and RecoveryOS entry (host/install_g4.h): host/install.c and host/vmrun.c compile
#                           its hooks only with it; set by tools/vmrun-install-g4-build.sh (the vmrun_g4 program, ships as its own .so, never through the main engine/app CMake build -- see the
#                           source-location check below) and by tools/ui-runner-vm-g4-install-build.sh (the ui_runner_vm_g4 program: the same G4 hooks combined with host/ui_runner_vm.c's own
#                           frame-sharing loop instead of host/vmrun.c's one-shot screenshot; host/ui_runner_vm.c itself is untouched and keeps its own installer=0 contract, checked separately by
#                           tools/check-runner-vm-safe.sh). Neither is part of the main engine/app CMake build; see the
#                           source-location check below)
#   HV_INSTALL_G5           the G5 diagnostic of the install entry point through TSS, the DFU->Recovery firmware transfer, RecoveryOS entry and into restore_device() itself (host/install_g5.h):
#                           host/install.c and host/vmrun.c compile its hooks only with it; set only by tools/vmrun-install-g5-build.sh (the vmrun_g5 program this ships as its own .so, never through the
#                           main engine/app CMake build -- see the source-location check below)
#   HV_INSTALL_G6           the G6 diagnostic of the install entry point through TSS, the DFU->Recovery firmware transfer, RecoveryOS entry, the restored handshake and into the Reverse Proxy attempt and
#                           the restore-options plist construction (host/install_g6.h): host/install.c and host/vmrun.c compile its hooks only with it; set only by tools/vmrun-install-g6-build.sh (the
#                           vmrun_g6 program this ships as its own .so, never through the main engine/app CMake build -- see the source-location check below)
#   FLAG_STOP_BEFORE_ROOTFS  (third_party/idevicerestore/src/hv_stop.h) is compiled into the Android restore stack but only host/restore_preflight.c ever sets it
#   FLAG_STOP_AFTER_DFU_RECOVERY  (third_party/idevicerestore/src/hv_stop.h) is compiled into the Android restore stack but only host/install_g3.c (through hv_g3_extra_flags) ever sets it
#   FLAG_STOP_AFTER_RECOVERYOS_ENTRY  (third_party/idevicerestore/src/hv_stop.h) is compiled into the Android restore stack but only host/install_g4.c (through hv_g4_extra_flags) ever sets it
#   FLAG_STOP_AFTER_RESTORED_HANDSHAKE  (third_party/idevicerestore/src/hv_stop.h) is compiled into the Android restore stack but only host/install_g5.c (through hv_g5_extra_flags) ever sets it
#   FLAG_STOP_AFTER_RESTORE_OPTIONS  (third_party/idevicerestore/src/hv_stop.h) is compiled into the Android restore stack but only host/install_g6.c (through hv_g6_extra_flags) ever sets it
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
  [ -f "$f" ] && grep -q "FLAG_STOP_AFTER_DFU_RECOVERY" "$f" && bad "$f sets the stop-B test flag"
  [ -f "$f" ] && grep -q "FLAG_STOP_AFTER_RECOVERYOS_ENTRY" "$f" && bad "$f sets the stop-C test flag"
  [ -f "$f" ] && grep -q "FLAG_STOP_AFTER_RESTORED_HANDSHAKE" "$f" && bad "$f sets the stop-D test flag"
  [ -f "$f" ] && grep -q "FLAG_STOP_AFTER_RESTORE_OPTIONS" "$f" && bad "$f sets the stop-E test flag"
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
# the G2 switch: only its own build script may set it (not the main engine/app CMake build, not iOS, not the Makefile; the CI workflow only calls the script by
# name and never names the switch itself, so it needs no exemption here -- same as HV_INSTALL_G0 and tools/vmrun-install-g0-build.sh)
if grep -rln "DHV_INSTALL_G2" tools android .github ios Makefile 2>/dev/null | grep -v "^tools/vmrun-install-g2-build.sh$\|^tools/check-no-test-flags.sh$" > /dev/null; then bad "something other than tools/vmrun-install-g2-build.sh sets -DHV_INSTALL_G2"; fi
# only host/install_g2.c sets the real TSS hosts and the signing-only stop; host/install.c keeps the plain flag-free source and reaches it through hv_g2_extra_flags
grep -q "hv_g2_extra_flags" host/install.c && ! grep -q "FLAG_SHSHONLY" host/install.c || bad "host/install.c does not go through the G2 hook, or names the signing-only flag"
# the G2 program is its own binary (vmrun_g2 -> libvmrung2.so), never part of the main engine/app's own CMake source list
if grep -rn "install_g2" android/CMakeLists.txt CMakeLists.txt 2>/dev/null | grep . > /dev/null; then bad "the main engine/app CMake build mentions host/install_g2"; fi
# the G3 switch: only its own build script may set it (not the main engine/app CMake build, not iOS, not the Makefile; the CI workflow only calls the script by
# name and never names the switch itself, so it needs no exemption here -- same as HV_INSTALL_G2 and tools/vmrun-install-g2-build.sh)
if grep -rln "DHV_INSTALL_G3" tools android .github ios Makefile 2>/dev/null | grep -v "^tools/vmrun-install-g3-build.sh$\|^tools/check-no-test-flags.sh$" > /dev/null; then bad "something other than tools/vmrun-install-g3-build.sh sets -DHV_INSTALL_G3"; fi
# only host/install_g3.c sets the DFU->Recovery stop; host/install.c keeps the plain flag-free source (checked above) and reaches it through hv_g3_extra_flags
grep -q "hv_g3_extra_flags" host/install.c && ! grep -q "FLAG_STOP_AFTER_DFU_RECOVERY" host/install.c || bad "host/install.c does not go through the G3 hook, or names the stop-B flag"
# the G3 program is its own binary (vmrun_g3 -> libvmrung3.so), never part of the main engine/app's own CMake source list
if grep -rn "install_g3" android/CMakeLists.txt CMakeLists.txt 2>/dev/null | grep . > /dev/null; then bad "the main engine/app CMake build mentions host/install_g3"; fi
# the G4 switch: only its own two build scripts may set it (not the main engine/app CMake build, not iOS, not the Makefile; the CI workflow only calls a
# script by name and never names the switch itself, so it needs no exemption here -- same as HV_INSTALL_G3 and tools/vmrun-install-g3-build.sh).
# tools/ui-runner-vm-g4-install-build.sh is a SECOND, separate test-only diagnostic (host/ui_runner_vm_g4.c -> ui_runner_vm_g4, not vmrun_g4): it combines
# the same G4 install hooks with host/ui_runner_vm.c's own frame-sharing loop to test whether G4's AppleLogo/RecoveryOS transfer reaches the shared-memory
# display; host/ui_runner_vm.c itself is untouched and still forces installer=0 (checked by tools/check-runner-vm-safe.sh, unaffected by this).
if grep -rln "DHV_INSTALL_G4" tools android .github ios Makefile 2>/dev/null | grep -v "^tools/vmrun-install-g4-build.sh$\|^tools/ui-runner-vm-g4-install-build.sh$\|^tools/check-no-test-flags.sh$" > /dev/null; then bad "something other than tools/vmrun-install-g4-build.sh or tools/ui-runner-vm-g4-install-build.sh sets -DHV_INSTALL_G4"; fi
# only host/install_g4.c sets the RecoveryOS-entry stop; host/install.c keeps the plain flag-free source (checked above) and reaches it through hv_g4_extra_flags
grep -q "hv_g4_extra_flags" host/install.c && ! grep -q "FLAG_STOP_AFTER_RECOVERYOS_ENTRY" host/install.c || bad "host/install.c does not go through the G4 hook, or names the stop-C flag"
# the G4 program is its own binary (vmrun_g4 -> libvmrung4.so), never part of the main engine/app's own CMake source list
if grep -rn "install_g4" android/CMakeLists.txt CMakeLists.txt 2>/dev/null | grep . > /dev/null; then bad "the main engine/app CMake build mentions host/install_g4"; fi
# the G5 switch: only its own build script may set it (not the main engine/app CMake build, not iOS, not the Makefile; the CI workflow only calls the script by
# name and never names the switch itself, so it needs no exemption here -- same as HV_INSTALL_G4 and tools/vmrun-install-g4-build.sh)
if grep -rln "DHV_INSTALL_G5" tools android .github ios Makefile 2>/dev/null | grep -v "^tools/vmrun-install-g5-build.sh$\|^tools/check-no-test-flags.sh$" > /dev/null; then bad "something other than tools/vmrun-install-g5-build.sh sets -DHV_INSTALL_G5"; fi
# only host/install_g5.c sets the restored-handshake stop; host/install.c keeps the plain flag-free source (checked above) and reaches it through hv_g5_extra_flags
grep -q "hv_g5_extra_flags" host/install.c && ! grep -q "FLAG_STOP_AFTER_RESTORED_HANDSHAKE" host/install.c || bad "host/install.c does not go through the G5 hook, or names the stop-D flag"
# the G5 program is its own binary (vmrun_g5 -> libvmrung5.so), never part of the main engine/app's own CMake source list
if grep -rn "install_g5" android/CMakeLists.txt CMakeLists.txt 2>/dev/null | grep . > /dev/null; then bad "the main engine/app CMake build mentions host/install_g5"; fi
# the G6 switch: only its own build script may set it (not the main engine/app CMake build, not iOS, not the Makefile; the CI workflow only calls the script by
# name and never names the switch itself, so it needs no exemption here -- same as HV_INSTALL_G5 and tools/vmrun-install-g5-build.sh)
if grep -rln "DHV_INSTALL_G6" tools android .github ios Makefile 2>/dev/null | grep -v "^tools/vmrun-install-g6-build.sh$\|^tools/check-no-test-flags.sh$" > /dev/null; then bad "something other than tools/vmrun-install-g6-build.sh sets -DHV_INSTALL_G6"; fi
# only host/install_g6.c sets the restore-options stop; host/install.c keeps the plain flag-free source (checked above) and reaches it through hv_g6_extra_flags
grep -q "hv_g6_extra_flags" host/install.c && ! grep -q "FLAG_STOP_AFTER_RESTORE_OPTIONS" host/install.c || bad "host/install.c does not go through the G6 hook, or names the stop-E flag"
# the G6 program is its own binary (vmrun_g6 -> libvmrung6.so), never part of the main engine/app's own CMake source list
if grep -rn "install_g6" android/CMakeLists.txt CMakeLists.txt 2>/dev/null | grep . > /dev/null; then bad "the main engine/app CMake build mentions host/install_g6"; fi
# the install disk is created or reset only by an explicit, separately approved step: no source of the app calls those functions yet (tests excluded)
if grep -rn "hv_install_disk_create\|hv_install_disk_reset" host devices core android/app/src --include=*.c --include=*.h --include=*.m --include=*.java --include=*.cpp 2>/dev/null | grep -v "^host/disk_policy\.[ch]:" | grep -v "^host/g7_runner_real\.c:" > /dev/null; then bad "something calls hv_install_disk_create / hv_install_disk_reset"; fi
# G7 skeleton (host/g7_preflight.*, host/runner_g7_core.*): not part of any shipped build, its execution stages are compiled only by the test script, and nothing but tests calls them
if grep -rn "HV_G7_MOCK_EXECUTION" . --include=* 2>/dev/null | grep -v "^./\.git/" | grep -v -E "^./(host/runner_g7_core\.[ch]|host/g7_probe\.c|tests/test_g7_runner\.c|tools/g7-preflight-test\.sh|tools/g7-probe-test\.sh|tools/g7-runner-test\.sh|tools/check-g7-probe-safe\.sh|tools/check-no-test-flags\.sh):" | grep . > /dev/null; then bad "HV_G7_MOCK_EXECUTION is used outside the G7 test build"; fi
if grep -rnE "HV_G7_(MIN_FREE_BYTES|ROM_SHA256_HEX|MIN_WRITTEN_BYTES)" . 2>/dev/null | grep -v "^./\\.git/" | grep -v -E "^./(host/(g7_preflight|g7_restore)\\.[ch]|tests/test_g7_[a-z]+\\.c|tools/g7-preflight-test\\.sh|tools/g7-probe-test\\.sh|tools/g7-runner-test\\.sh|tools/g7-restore-test\\.sh|tools/check-g7-probe-safe\\.sh|tools/check-no-test-flags\\.sh):" | grep . > /dev/null; then bad "a G7 test-only constant (free-space floor, ROM hash) is set outside the G7 tests"; fi
# The skeleton itself (preflight/runner core) is named by no shipped build or app source; the probe program (g7_probe) is named only by its own build script and its two app files (tools/check-g7-probe-launcher-source.sh holds those).
if grep -rnE "g7_preflight|runner_g7_core|g7-preflight" android/CMakeLists.txt CMakeLists.txt android/app/src android/app/build.gradle* tools/*build*.sh 2>/dev/null | grep -v -E '^tools/(g7-probe-build|g7-runner-build|restore-g7-build)\.sh:' | grep . > /dev/null; then bad "the G7 skeleton is mentioned by a shipped build or app source"; fi
if grep -rnE "g7_probe|g7-probe" android/CMakeLists.txt CMakeLists.txt android/app/src android/app/build.gradle* tools/*build*.sh 2>/dev/null | grep -v -E '^tools/(g7-probe-build|g7-runner-build)\.sh:|^android/app/src/main/java/dev/muns/hengeprobe/G7Probe(Launcher|Activity)\.java:|^android/app/src/main/(AndroidManifest\.xml|java/dev/muns/hengeprobe/MainActivity\.java):' | grep . > /dev/null; then bad "g7_probe is mentioned outside its own build script and app files"; fi
if grep -rnE "hv_g7_(pair_begin|pair_finish|pair_quarantine|make_config)[[:space:]]*\\(" host devices core android/app/src --include=*.c --include=*.h --include=*.m --include=*.java --include=*.cpp 2>/dev/null | grep -v -E "^host/(g7_preflight|runner_g7_core|g7_restore|g7_runner_real)\\.[ch]:" | grep . > /dev/null; then bad "something outside the G7 skeleton calls the G7 pair functions"; fi
if grep -rnE "hv_g7_run[[:space:]]*\\(" host devices core android/app/src --include=*.c --include=*.h --include=*.m --include=*.java --include=*.cpp 2>/dev/null | grep -v -E "^host/(runner_g7_core\\.[ch]|g7_probe\\.c|g7_runner\\.c):" | grep . > /dev/null; then bad "something besides host/g7_probe.c and host/g7_runner.c calls hv_g7_run"; fi
# G7 EXECUTION build (host/g7_runner_real.c, -DHV_G7_REAL_EXECUTION, the restore stack without stop switches): built only by tools/g7-runner-build.sh, named by no APK step and no app source
if grep -rn "HV_G7_REAL_EXECUTION" . 2>/dev/null | grep -v "^./\.git/" | grep -v -E "^./(host/(runner_g7_core\.[ch]|g7_runner\.c|g7_runner_real\.[ch])|tools/g7-runner-build\.sh|tools/check-no-test-flags\.sh|tools/check-g7-probe-safe\.sh):" | grep . > /dev/null; then bad "HV_G7_REAL_EXECUTION is used outside the G7 execution build"; fi
if grep -rnE "g7_runner_real|restore-g7-build|g7-runner-build\.sh[[:space:]]+real|HV_G7_RESTORE" android .github/workflows/android-apk.yml ios Makefile CMakeLists.txt 2>/dev/null | grep . > /dev/null; then bad "an APK/app/iOS build step names the G7 execution build"; fi
if grep -rnE "restore-g7-build|restore_g7" tools/*.sh .github 2>/dev/null | grep -v -E "^(tools/(restore-g7-build|g7-runner-build|check-restore-g7-build|check-no-test-flags)\.sh|\.github/workflows/android-engine\.yml):" | grep . > /dev/null; then bad "the G7 restore stack is named outside its own scripts and the engine CI"; fi
# G7 network allow-list (host/install_g7.c, -DHV_INSTALL_G7): compiled only by the G7 restore stack script; its entry point is called only from host/install.c under the macro; never in an APK/app build
if grep -rn "HV_INSTALL_G7" . 2>/dev/null | grep -v "^./\.git/" | grep -v -E "^./(host/(install\.c|install_g7\.[ch])|tools/(restore-g7-build|check-no-test-flags|g7-net-test)\.sh|tests/test_g7_net\.c):" | grep . > /dev/null; then bad "HV_INSTALL_G7 is used outside the G7 restore stack"; fi
if grep -rn "hv_g7_extra_flags" . 2>/dev/null | grep -v "^./\.git/" | grep -v -E "^./(host/(install\.c|install_g7\.[ch])|tests/test_g7_net\.c|tools/(g7-net-test|check-no-test-flags)\.sh):" | grep . > /dev/null; then bad "hv_g7_extra_flags is called outside host/install.c"; fi
if grep -rnE "install_g7|HV_INSTALL_G7" android .github/workflows/android-apk.yml ios Makefile CMakeLists.txt 2>/dev/null | grep . > /dev/null; then bad "an APK/app/iOS build names the G7 network allow-list"; fi
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
