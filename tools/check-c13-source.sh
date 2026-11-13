#!/bin/bash
# C1-3: source rules for the app side of the VM runner. A structural rule comes first (all process creation for libuirunnervm.so is in VmRunnerLauncher.java);
# these checks only back it up. They find direct uses; they cannot find a name that is built up at run time or a different way of starting a process.
# Usage: tools/check-c13-source.sh   [then, optionally, an APK:  tools/check-c13-source.sh <app-debug.apk>]
cd "$(dirname "$0")/.." || exit 1
J=android/app/src/main/java/dev/muns/hengeprobe; L=$J/VmRunnerLauncher.java
fail=0; bad() { echo "FAIL $*"; fail=1; }
[ -f "$L" ] || { echo "FAIL missing $L"; exit 1; }
# the switch: declared once, false, read only inside the launcher
n=$(grep -cE 'static final boolean START_ENABLED' $L); [ "$n" = 1 ] || bad "START_ENABLED must be declared once (found $n)"
grep -qE 'static final boolean START_ENABLED = false;' $L || bad "START_ENABLED must be false in this build"
x=$(grep -rn 'START_ENABLED' $J --include=*.java | grep -v "^$L:"); [ -z "$x" ] || { bad "START_ENABLED named outside the launcher:"; echo "$x"; }
# the process: one ProcessBuilder in the launcher; the check comes before it
n=$(grep -c 'new ProcessBuilder' $L); [ "$n" = 1 ] || bad "the launcher must have exactly one ProcessBuilder (found $n)"
l1=$(grep -n 'if (!START_ENABLED)' $L | head -1 | cut -d: -f1); l2=$(grep -n 'spawner.spawn(' $L | head -1 | cut -d: -f1)
{ [ -n "$l1" ] && [ -n "$l2" ] && [ "$l1" -lt "$l2" ]; } || bad "the START_ENABLED check must come before spawn (lines '$l1' '$l2')"
# nobody else names the runner file or the launcher's spawner, and the two screens create no process
x=$(grep -rnE 'uirunnervm|LIB_NAME' $J --include=*.java | grep -v "^$L:" | grep -v "^$J/VmRunnerActivity.java:.*VmRunnerLauncher.LIB_NAME"); [ -z "$x" ] || { bad "the VM runner file is named outside the launcher:"; echo "$x"; }
x=$(grep -nE 'ProcessBuilder|Runtime\.getRuntime|\.exec\(' $J/VmRunnerActivity.java); [ -z "$x" ] || { bad "VmRunnerActivity creates a process:"; echo "$x"; }
x=$(grep -nE 'getIntent|getExtra|getStringExtra|getBooleanExtra' $J/VmRunnerActivity.java); [ -z "$x" ] || { bad "VmRunnerActivity reads the Intent:"; echo "$x"; }
grep -A3 'VmRunnerActivity' android/app/src/main/AndroidManifest.xml | grep -q 'android:exported="false"' || bad "VmRunnerActivity must be exported=false"
# the APK, when given: both runners in it, both AArch64
if [ -n "${1:-}" ]; then
  A=$1; RE=${READELF:-readelf}; T=$(mktemp -d)
  for f in libuirunner.so libuirunnervm.so; do
    unzip -q -o "$A" "lib/arm64-v8a/$f" -d "$T" 2>/dev/null || { bad "$f is not in the APK"; continue; }
    m=$($RE -h "$T/lib/arm64-v8a/$f" | sed -n 's/^ *Machine: *//p'); case "$m" in *AArch64*) ;; *) bad "$f: ELF machine '$m'";; esac
  done
  rm -rf "$T"
fi
[ $fail = 0 ] && echo "OK  C1-3 source rules: START_ENABLED once and false, one ProcessBuilder after the check, runner file named only in VmRunnerLauncher, the info screen starts no process${1:+, APK has both runners (AArch64)}"
exit $fail
