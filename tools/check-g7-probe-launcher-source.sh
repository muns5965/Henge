#!/bin/bash
# Source rules for the app side of the G7 preflight (libg7probe.so = host/g7_probe.c). All process creation for it is in G7ProbeLauncher.java; these checks find direct uses,
# not names built at run time. The screen only looks: no writes, no ECID creation, no Intent extras, not exported. Usage: tools/check-g7-probe-launcher-source.sh
cd "$(dirname "$0")/.." || exit 1
J=android/app/src/main/java/dev/muns/hengeprobe; L=$J/G7ProbeLauncher.java; A=$J/G7ProbeActivity.java
fail=0; bad() { echo "FAIL $*"; fail=1; }
[ -f "$L" ] && [ -f "$A" ] || { echo "FAIL missing launcher or activity"; exit 1; }
n=$(grep -c 'new ProcessBuilder' $L); [ "$n" = 1 ] || bad "exactly one ProcessBuilder in the launcher (found $n)"
x=$(grep -rnE 'ProcessBuilder|Runtime\.getRuntime|\.exec\(' $J --include=*.java | grep -E 'G7Probe'| grep -v "^$L:"); [ -z "$x" ] || { bad "process creation in the G7 activity:"; echo "$x"; }
x=$(grep -rn 'libg7probe' $J --include=*.java | grep -v "^$L:" | grep -v "^$A:"); [ -z "$x" ] || { bad "the probe file is named outside its launcher/activity:"; echo "$x"; }
grep -q 'LIB_NAME = "libg7probe.so"' $L || bad "LIB_NAME must be libg7probe.so"
# no other program is started by name from these two files
x=$(grep -nE 'libuirunner|libvmrun|libidevice|libcurl' $L $A); [ -z "$x" ] || { bad "another program is named:"; echo "$x"; }
# nothing here creates or writes
x=$(grep -nE 'loadOrCreate|FileOutputStream|FileWriter|Files\.write|createNewFile|mkdirs?\(|\.delete\(|renameTo|RandomAccessFile|openFileOutput|hv_install_disk' $L $A); [ -z "$x" ] || { bad "a write or creation path:"; echo "$x"; }
# only the five options, no write switches
x=$(grep -nE '"--(installer|seed|aux-offset|ram|cpus|usb)' $L); [ -z "$x" ] || { bad "an option the probe must not get:"; echo "$x"; }
# the activity: not exported, no extras read, no logging of the ECID
grep -A3 'G7ProbeActivity' android/app/src/main/AndroidManifest.xml | grep -q 'android:exported="false"' || bad "G7ProbeActivity must be exported=false"
x=$(grep -nE 'getIntent|getExtras|getStringExtra|Log\.[a-z]\(' $A $L); [ -z "$x" ] || { bad "intent input or logging in the G7 screen/launcher:"; echo "$x"; }
# the ECID value is only ever passed through mask() on its way to the screen
grep -q 'mask(' $L || bad "output must pass through mask()"
[ $fail = 0 ] && echo "OK  G7 probe launcher source rules"
exit $fail
