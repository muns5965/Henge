#!/bin/bash
# Source rules for the app side of the G7 execution build (android/app/src/g7exec). Finds direct uses, not names built at run time.
# Usage: tools/check-g7exec-launcher-source.sh
cd "$(dirname "$0")/.." || exit 1
X=android/app/src/g7exec; XJ=$X/java/dev/muns/hengeprobe; MAIN=android/app/src/main
L=$XJ/G7ExecLauncher.java; A=$XJ/G7ExecActivity.java
fail=0; bad() { echo "FAIL $*"; fail=1; }
[ -f "$L" ] && [ -f "$A" ] && [ -f "$X/AndroidManifest.xml" ] || { echo "FAIL missing launcher, activity or manifest"; exit 1; }
# the committed switch is OFF, and the only public start passes exactly that constant
grep -qE '^\s*static final boolean START_ENABLED = false;' "$L" || bad "START_ENABLED must be the literal false"
[ "$(grep -c 'START_ENABLED' "$L" | tr -d ' ')" -le 6 ] || bad "START_ENABLED is used in too many places"
grep -q 'return startWith(REAL, G7ProbeLauncher::usableRomFile, START_ENABLED,' "$L" || bad "start() must pass the committed START_ENABLED"
[ "$(grep -c 'startWith(REAL' "$L")" = 1 ] || bad "REAL spawner is used in more than one place"
# one process creation, only here
n=$(grep -c 'new ProcessBuilder' "$L"); [ "$n" = 1 ] || bad "exactly one ProcessBuilder in the launcher (found $n)"
x=$(grep -rnE 'ProcessBuilder|Runtime\.getRuntime|\.exec\(' android/app/src --include=*.java | grep -E 'G7Exec' | grep -v "^$L:"); [ -z "$x" ] || { bad "process creation in the G7 execution activity:"; echo "$x"; }
# the binary and the launcher are named only in the g7exec source set
x=$(grep -rln 'libg7runner\|G7ExecLauncher\|G7ExecActivity\|g7exec' $MAIN 2>/dev/null); [ -z "$x" ] || { bad "the main source set names the G7 execution build:"; echo "$x"; }
grep -q 'LIB_NAME = "libg7runner.so"' "$L" || bad "LIB_NAME must be libg7runner.so"
# the ECID never travels on a command line and is never printed
x=$(grep -nE '"--ecid"|--ecid ' "$L"); [ -z "$x" ] || { bad "an --ecid option:"; echo "$x"; }
grep -q 'mask(' "$L" || bad "output must pass through mask()"
# the options: exactly the ones the runner needs; nothing that lowers a check
x=$(grep -nE '"--(installer|seed|aux-offset|min|force|skip|allow)' "$L"); [ -z "$x" ] || { bad "an option the runner must not get:"; echo "$x"; }
# nothing here deletes, moves or resets an install file
x=$(grep -nE '\.delete\(|renameTo|Files\.(delete|move)|RandomAccessFile\(.*"rw"|hv_install_disk' "$L" "$A"); [ -z "$x" ] || { bad "a delete/move/rewrite path:"; echo "$x"; }
# writes: only a NEW run log per run (CREATE_NEW: never appended to, never overwritten) and the work directory
[ "$(grep -c 'Files.newOutputStream' "$L")" = 1 ] || bad "exactly one output stream (the run log)"
grep -q 'Files.newOutputStream(log.toPath(), StandardOpenOption.CREATE_NEW' "$L" || bad "the run log must be opened with CREATE_NEW"
x=$(grep -nE 'FileOutputStream|FileWriter|StandardOpenOption.(APPEND|TRUNCATE_EXISTING)' "$L"); [ -z "$x" ] || { bad "a way to append to or overwrite a file:"; echo "$x"; }
# the run state is process-wide (not in the Activity)
grep -q 'static volatile Run CURRENT' "$L" || bad "the run state must be process-wide (CURRENT)"
x=$(grep -nE 'G7ExecLauncher\.Run [a-z]+;|private .*Run run' "$A"); [ -z "$x" ] || { bad "the activity keeps run state in a field:"; echo "$x"; }
x=$(grep -nE 'FileOutputStream|FileWriter|Files\.write|createNewFile|openFileOutput' "$A"); [ -z "$x" ] || { bad "the activity writes files:"; echo "$x"; }
# the activity: no intent input, no logging, starts only through the launcher
x=$(grep -nE 'getIntent|getExtras|getStringExtra|Log\.[a-z]\(' "$A" "$L"); [ -z "$x" ] || { bad "intent input or logging:"; echo "$x"; }
grep -q 'G7ExecLauncher.start(' "$A" || bad "the activity must start only through G7ExecLauncher.start"
# the execution manifest overlay: one activity, only the activity of this build
[ "$(grep -c '<activity' $X/AndroidManifest.xml)" = 1 ] || bad "the g7exec manifest declares exactly one activity"
# the general manifest does not mention it
grep -q 'G7Exec' $MAIN/AndroidManifest.xml && bad "the general manifest names G7Exec"
# the build type exists and is the only way to include the source set
grep -q 'g7exec {' android/app/build.gradle || bad "android/app/build.gradle lacks the g7exec build type"
# no binary is committed into the source set
x=$(find $X/jniLibs -type f ! -name .gitkeep 2>/dev/null); [ -z "$x" ] || { bad "a binary is committed under $X/jniLibs:"; echo "$x"; }
[ $fail = 0 ] && echo "OK  G7 execution launcher source rules (switch off, one process creation, no ECID on a command line, no delete/move, not in the general source set)"
exit $fail
