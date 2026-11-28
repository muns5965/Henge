#!/bin/bash
# Every native program the app's Java code names ("lib....so") must be classified, so a new gap cannot appear silently (c802414 lacked libvmrun.so although libipswdl.so/libromextract.so had just been added).
#   SHIPPED   built by the G7 workflows (tools/g7-install-libs-build.sh, the runner/clone-boot steps) and required in the APK by tools/check-g7exec-apk.sh
#   OPTIONAL  built by the G7 workflows, not required
#   NOT_SHIPPED  buttons of the general test app that the G7 APK does not carry (diagnostics, install G0-G6 with stop switches, UI runners); their buttons say the file is not in the APK
# First-time setup path on a new device: IPSW download (libipswdl) -> ROM extract (libromextract) -> ROM test / ECID file (libvmrun, MainActivity.runVmrun) -> G7 install (libg7runner) -> clone boot (libg7cloneboot).
# Usage: tools/check-g7-native-manifest.sh
cd "$(dirname "$0")/.."
SHIPPED="libvmrun.so libromextract.so libipswdl.so libg7runner.so libg7cloneboot.so"
OPTIONAL="libcurltool.so libg7pipelinerepro.so libnetprobe.so"
NOT_SHIPPED="libvmrung0.so libvmrung2.so libvmrung3.so libvmrung4.so libvmrung5.so libvmrung6.so libvmrunirecv.so libvmruntss.so libvmruntsslpol.so libvmrunpreflight.so librestoreselftest.so libbmprobe.so libuirunner.so libuirunnervm.so libuirunnervmg4.so libuirunnervmi.so libg7probe.so"
fail=0
for n in $(grep -rhoE '"lib[a-z0-9_]+\.so"' android/app/src --include=*.java | tr -d '"' | sort -u); do
  case " $SHIPPED $OPTIONAL $NOT_SHIPPED " in *" $n "*) ;; *) echo "FAIL $n is named in the Java sources but is not classified in tools/check-g7-native-manifest.sh"; fail=1;; esac
done
for n in $SHIPPED; do
  grep -q "$n" tools/check-g7exec-apk.sh || [ "$n" = libg7runner.so ] || [ "$n" = libg7cloneboot.so ] || { echo "FAIL $n is SHIPPED here but tools/check-g7exec-apk.sh does not require it"; fail=1; }
  grep -rq "$n" tools/g7-install-libs-build.sh tools/check-g7exec-apk.sh .github/workflows/android-g7lagfx.yml || { echo "FAIL $n is SHIPPED but no G7 build step names it"; fail=1; }
done
[ $fail = 0 ] && echo "OK  every native program named by the Java sources is classified (shipped: $SHIPPED)"
exit $fail
