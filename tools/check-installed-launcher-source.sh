#!/bin/bash
# Source rules for the app side of the installed-disk boot runner (libuirunnervmi.so = host/ui_runner_vm_installed.c),
# mirroring tools/check-g4ui-launcher-source.sh. Structural rule first: all process creation for that runner is in
# VmRunnerInstalledLauncher.java; these checks back it up (they find direct uses, not names built at run time).
# START_ENABLED is the literal false in this build; turning it on, or loosening this script, needs a fresh explicit approval.
# Usage: tools/check-installed-launcher-source.sh
cd "$(dirname "$0")/.." || exit 1
J=android/app/src/main/java/dev/muns/hengeprobe; L=$J/VmRunnerInstalledLauncher.java
fail=0; bad() { echo "FAIL $*"; fail=1; }
[ -f "$L" ] || { echo "FAIL missing $L"; exit 1; }
n=$(grep -cE 'static final boolean START_ENABLED' $L); [ "$n" = 1 ] || bad "START_ENABLED must be declared once (found $n)"
grep -qE 'static final boolean START_ENABLED = false;' $L || bad "START_ENABLED must be the literal false in this build"
x=$(grep -rn 'START_ENABLED' $J --include=*.java | grep -v "^$L:" | grep -v "^$J/VmRunnerLauncher.java:" | grep -v "^$J/VmRunnerG4Launcher.java:"); [ -z "$x" ] || { bad "START_ENABLED named outside the launchers:"; echo "$x"; }
n=$(grep -c 'new ProcessBuilder' $L); [ "$n" = 1 ] || bad "the launcher must have exactly one ProcessBuilder (found $n)"
l1=$(grep -n 'if (!START_ENABLED)' $L | head -1 | cut -d: -f1); l2=$(grep -n 'spawner.spawn(' $L | head -1 | cut -d: -f1)
{ [ -n "$l1" ] && [ -n "$l2" ] && [ "$l1" -lt "$l2" ]; } || bad "the START_ENABLED check must come before spawn (lines '$l1' '$l2')"
# the switch check is the first statement of startWith
first=$(awk '/static Result startWith\(/{f=1;next} f&&NF{print;exit}' $L)
echo "$first" | grep -q 'if (!START_ENABLED)' || bad "the first statement of startWith must be the START_ENABLED check (found: $first)"
# nobody else names the runner file
x=$(grep -rnE 'uirunnervmi' $J --include=*.java | grep -v "^$L:"); [ -z "$x" ] || { bad "the installed runner file is named outside the launcher:"; echo "$x"; }
# no blank-image fallback anywhere in the installed-disk code
x=$(grep -nE 'blank_disk|blank_aux' $J/VmRunnerInstalledActivity.java $J/VmInstalledDiag.java); [ -z "$x" ] || { bad "installed-disk screens must not name blank images:"; echo "$x"; }
grep -q 'startsWith("blank_")' $L || bad "the launcher must refuse blank_ images"
# the contract with the native runner
R=host/ui_runner_vm_installed.c
nat=$(grep -o '"ui_runner_vm_installed: [^"]*are required' $R | grep -o -- '--[a-z-]*' | sort -u)
nat=$(printf '%s\n--ecid\n' "$nat" | sort -u)
accepted=$(sed -n '/Arguments:/,/Standard input:/p' $R | grep -o -- '--[a-z-]*' | sort -u)
jav=$(grep -o 'c.add("--[a-z-]*")' $L | grep -o -- '--[a-z-]*' | sort -u)
for o in $nat; do echo "$jav" | grep -qFx -- "$o" || bad "runner requires '$o' but buildCommand does not add it"; done
for o in $jav; do echo "$accepted" | grep -qFx -- "$o" || bad "buildCommand adds '$o', which $R's Arguments line does not list as accepted"; done
for o in --installer --net --write --rw --readonly --no-readonly; do echo "$jav" | grep -qFx -- "$o" && bad "buildCommand must never add $o"; done
if echo "$jav" | grep -qx -- '--seconds'; then
  n=$(grep -oE 'static final int RUN_SECONDS = [0-9]+;' $L | grep -oE '[0-9]+')
  { [ -n "$n" ] && [ "$n" -gt 0 ] && [ "$n" -le 600 ]; } || bad "RUN_SECONDS must be a literal int in (0, 600], found '$n'"
else bad "buildCommand must add --seconds (a time limit)"; fi
# the readiness screen and its report: no process, no Intent input, no writes, never name the other runners/screens or start()
A=$J/VmRunnerInstalledActivity.java; D=$J/VmInstalledDiag.java
for f in $A $D; do
  [ -f "$f" ] || { bad "missing $f"; continue; }
  x=$(grep -nE 'ProcessBuilder|Runtime\.getRuntime|\.exec\(|getIntent|getExtra|getStringExtra|getBooleanExtra|FileOutputStream|FileWriter|Files\.write|createNewFile|\.mkdir|\.delete\(|renameTo' $f); [ -z "$x" ] || { bad "$f creates a process, reads the Intent or writes a file:"; echo "$x"; }
  x=$(grep -nE 'VmRunnerLauncher|VmRunnerG4Launcher|VmDisplayActivity|VmDisplayG4Activity|VmRunnerInstalledLauncher\.start\(|VmEcid\.loadOrCreate' $f); [ -z "$x" ] || { bad "$f names another runner/screen, start(), or loadOrCreate:"; echo "$x"; }
done
grep -A3 'VmRunnerInstalledActivity' android/app/src/main/AndroidManifest.xml | grep -q 'android:exported="false"' || bad "VmRunnerInstalledActivity must be exported=false"
[ $fail = 0 ] && echo "OK  installed-disk launcher source rules: START_ENABLED declared once as the literal false and checked first, one ProcessBuilder after it, options match what ui_runner_vm_installed requires/accepts (no loosening option), RUN_SECONDS bounded, blank images refused, runner file named only in the launcher, the [A-INST] screen and report start nothing, write nothing and cannot name the other runners"
exit $fail
