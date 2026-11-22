#!/bin/bash
# G4 UI Phase 2: source rules for the app side of the G4 diagnostic launcher, mirroring tools/check-c13-source.sh's
# approach for VmRunnerLauncher. A structural rule comes first (all process creation for libuirunnervmg4.so is in
# VmRunnerG4Launcher.java); these checks only back it up. They find direct uses; they cannot find a name that is
# built up at run time or a different way of starting a process.
#
# Unlike check-c13-source.sh, this build's own switch is a literal false: running host/ui_runner_vm_g4.c always
# makes a real Apple TSS request and drives the guest through DFU->Recovery->RecoveryOS the moment it starts (see
# VmRunnerG4Launcher.java's own header comment) -- there is no "observation only" mode for it to fall back to. So
# this script additionally requires the literal to be false, not just a literal; a change flipping it to true is
# refused here and needs a change to this script itself, which is its own separate, explicit approval.
#
# Usage: tools/check-g4ui-launcher-source.sh
cd "$(dirname "$0")/.." || exit 1
J=android/app/src/main/java/dev/muns/hengeprobe; L=$J/VmRunnerG4Launcher.java
fail=0; bad() { echo "FAIL $*"; fail=1; }
[ -f "$L" ] || { echo "FAIL missing $L"; exit 1; }
# the switch: declared once, a literal false (never computed, never true), read only inside the launcher
n=$(grep -cE 'static final boolean START_ENABLED' $L); [ "$n" = 1 ] || bad "START_ENABLED must be declared once (found $n)"
grep -qE 'static final boolean START_ENABLED = false;' $L || bad "START_ENABLED must be the literal false in this build (flipping it to true needs a change to this script too)"
x=$(grep -rn 'START_ENABLED' $J --include=*.java | grep -v "^$L:" | grep -v "^$J/VmRunnerLauncher.java:"); [ -z "$x" ] || { bad "START_ENABLED named outside the launcher (or VmRunnerLauncher's own, unrelated switch):"; echo "$x"; }
# the process: one ProcessBuilder in the launcher; the check comes before it
n=$(grep -c 'new ProcessBuilder' $L); [ "$n" = 1 ] || bad "the launcher must have exactly one ProcessBuilder (found $n)"
l1=$(grep -n 'if (!START_ENABLED)' $L | head -1 | cut -d: -f1); l2=$(grep -n 'spawner.spawn(' $L | head -1 | cut -d: -f1)
{ [ -n "$l1" ] && [ -n "$l2" ] && [ "$l1" -lt "$l2" ]; } || bad "the START_ENABLED check must come before spawn (lines '$l1' '$l2')"
# nobody else names the runner file
x=$(grep -rnE 'uirunnervmg4|VmRunnerG4Launcher\.LIB_NAME' $J --include=*.java | grep -v "^$L:"); [ -z "$x" ] || { bad "the G4 UI runner file is named outside the launcher:"; echo "$x"; }
# the contract with the native runner: every option host/ui_runner_vm_g4.c requires (--shm --rom --disk --aux
# --ipsw, plus --ecid and --workdir, worded separately in its own "are required"/"Arguments:" lines) must be among
# the options buildCommand adds, and buildCommand may add nothing beyond what the runner's own "Arguments:" line
# says it accepts in total.
R=host/ui_runner_vm_g4.c
nat=$(grep -o '"ui_runner_vm_g4: [^"]*are required' $R | grep -o -- '--[a-z-]*' | sort -u)
nat="$nat
--ecid
--workdir"
nat=$(echo "$nat" | sort -u)
# the "Arguments:" comment spans several source lines (unlike host/ui_runner_vm.c's single-line version), so this
# joins everything from "Arguments:" through "Standard input:" before pulling out option names.
accepted=$(sed -n '/Arguments:/,/Standard input:/p' $R | grep -o -- '--[a-z-]*' | sort -u)
jav=$(grep -o 'c.add("--[a-z-]*")' $L | grep -o -- '--[a-z-]*' | sort -u)
for o in $nat; do echo "$jav" | grep -qFx -- "$o" || bad "runner requires '$o' but VmRunnerG4Launcher.buildCommand does not add it"; done
for o in $jav; do echo "$accepted" | grep -qFx -- "$o" || bad "VmRunnerG4Launcher.buildCommand adds '$o', which host/ui_runner_vm_g4.c's own Arguments line does not list as accepted"; done
# if the launcher adds --seconds at all, it must be a literal, positive constant at least the proven G4 floor
# (MainActivity.java's own existing [G4] button: `if (g4 && secs < 460) secs = 460`) -- not 0 (that would disable
# ui_runner_vm_g4's own time limit entirely) and not some value computed at run time this script cannot see.
if echo "$jav" | grep -qx -- '--seconds'; then
  n=$(grep -oE 'static final int RUN_SECONDS = [0-9]+;' $L | grep -oE '[0-9]+')
  { [ -n "$n" ] && [ "$n" -ge 460 ]; } || bad "VmRunnerG4Launcher.RUN_SECONDS must be a literal int >= 460 (the proven G4 floor), found '$n'"
fi
# Phase 3's result-judgment classes (VmRunnerG4Verdict.java, VmRunnerG4Run.java) only ever observe a Process
# the caller already made -- they must never construct one themselves or read the launcher's switch, so that
# nothing in that judgment path could ever become a second way to start real execution.
for f in VmRunnerG4Verdict.java VmRunnerG4Run.java; do
  P=$J/$f
  [ -f "$P" ] || continue
  x=$(grep -n 'new ProcessBuilder\|START_ENABLED' $P); [ -z "$x" ] || { bad "$f must never create a process or read START_ENABLED:"; echo "$x"; }
done
[ $fail = 0 ] && echo "OK  G4 UI launcher source rules: START_ENABLED declared once as the literal false, one ProcessBuilder after the check, the options match what ui_runner_vm_g4 requires/accepts, runner file named only in VmRunnerG4Launcher, the Phase 3 judgment classes create no process"
exit $fail
