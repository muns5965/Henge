#!/bin/bash
# The app side of the G7 execution build (android/app/src/g7exec/.../G7ExecLauncher.java) on a plain JVM with fake processes: nothing is started, nothing real is touched. Usage: tools/g7-exec-launcher-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; J=android/app/src/main/java/dev/muns/hengeprobe; X=android/app/src/g7exec/java/dev/muns/hengeprobe
rm -rf "$OUT"; mkdir -p "$OUT"
javac -encoding UTF-8 -d "$OUT" $X/G7ExecLauncher.java $X/G7Isolation.java $X/G7Storage.java $J/G7ProbeLauncher.java $J/VmRomCheck.java $J/RomSlice.java $J/VmEcid.java $J/VmRunnerInstalledLauncher.java tests/java/G7ExecLauncherTest.java tests/java/dev/muns/hengeprobe/BuildConfig.java 2> "$OUT/javac.log" || { echo "FAILED javac"; head -30 "$OUT/javac.log"; exit 1; }
java -cp "$OUT" dev.muns.hengeprobe.G7ExecLauncherTest; R1=$?

# the g7start build type: the same launcher against the switch-ON stand-in of BuildConfig; the typed approval and the preflight still refuse everything that is not complete
O2="$OUT-starton"; rm -rf "$O2"; mkdir -p "$O2"
javac -encoding UTF-8 -d "$O2" $X/G7ExecLauncher.java $X/G7Isolation.java $X/G7Storage.java $J/G7ProbeLauncher.java $J/VmRomCheck.java $J/RomSlice.java $J/VmEcid.java $J/VmRunnerInstalledLauncher.java tests/java/G7ExecStartOnTest.java tests/java-starton/dev/muns/hengeprobe/BuildConfig.java 2> "$O2/javac.log" || { echo "FAILED javac (start-on)"; head -30 "$O2/javac.log"; exit 1; }
java -cp "$O2" dev.muns.hengeprobe.G7ExecStartOnTest; R2=$?

# the isolation of an existing install pair (renames only), on mock files
O3="$OUT-isolation"; rm -rf "$O3"; mkdir -p "$O3"
javac -encoding UTF-8 -d "$O3" $X/G7ExecLauncher.java $X/G7Isolation.java $X/G7Storage.java $J/G7ProbeLauncher.java $J/VmRomCheck.java $J/RomSlice.java $J/VmEcid.java $J/VmRunnerInstalledLauncher.java tests/java/G7ExecIsolationTest.java tests/java/dev/muns/hengeprobe/BuildConfig.java 2> "$O3/javac.log" || { echo "FAILED javac (isolation)"; head -30 "$O3/javac.log"; exit 1; }
java -cp "$O3" dev.muns.hengeprobe.G7ExecIsolationTest; R3=$?
# the network probe's app side (G7NetProbe: one program, no arguments, own log, cancel), with fake processes; G7NetState (Android classes) is compiled by the APK build only
O4="$OUT-netprobe"; rm -rf "$O4"; mkdir -p "$O4"
javac -encoding UTF-8 -d "$O4" $X/G7NetProbe.java $X/G7ExecLauncher.java $X/G7Isolation.java $X/G7Storage.java $J/G7ProbeLauncher.java $J/VmRomCheck.java $J/RomSlice.java $J/VmEcid.java $J/VmRunnerInstalledLauncher.java tests/java/G7NetProbeTest.java tests/java/dev/muns/hengeprobe/BuildConfig.java 2> "$O4/javac.log" || { echo "FAILED javac (netprobe)"; head -30 "$O4/javac.log"; exit 1; }
java -cp "$O4" dev.muns.hengeprobe.G7NetProbeTest; R4=$?
# the storage list and the deletion of failed install pairs (mock files only)
O5="$OUT-storage"; rm -rf "$O5"; mkdir -p "$O5"
javac -encoding UTF-8 -d "$O5" $X/G7ExecLauncher.java $X/G7Isolation.java $X/G7Storage.java $J/G7ProbeLauncher.java $J/VmRomCheck.java $J/RomSlice.java $J/VmEcid.java $J/VmRunnerInstalledLauncher.java tests/java/G7ExecStorageTest.java tests/java/dev/muns/hengeprobe/BuildConfig.java 2> "$O5/javac.log" || { echo "FAILED javac (storage)"; head -30 "$O5/javac.log"; exit 1; }
java -cp "$O5" dev.muns.hengeprobe.G7ExecStorageTest; R5=$?
# the exit-record formatter (pure, no Android classes)
O6="$OUT-exitinfo"; rm -rf "$O6"; mkdir -p "$O6"
javac -encoding UTF-8 -d "$O6" $X/G7ExitInfo.java tests/java/G7ExitInfoTest.java 2> "$O6/javac.log" || { echo "FAILED javac (exitinfo)"; head -30 "$O6/javac.log"; exit 1; }
java -cp "$O6" dev.muns.hengeprobe.G7ExitInfoTest; R6=$?
javac -encoding UTF-8 -d "$O6" $X/G7ExitTrace.java tests/java/G7ExitTraceTest.java 2>> "$O6/javac.log" || { echo "FAILED javac (exittrace)"; head -30 "$O6/javac.log"; exit 1; }
java -cp "$O6" dev.muns.hengeprobe.G7ExitTraceTest; R7=$?
# the sparse clone of the install pair (mock files only)
O8="$OUT-clone"; rm -rf "$O8"; mkdir -p "$O8"
javac -encoding UTF-8 -d "$O8" $X/G7ExecLauncher.java $X/G7Isolation.java $X/G7Storage.java $X/G7Clone.java $X/G7CloneProgress.java $J/G7ProbeLauncher.java $J/VmRomCheck.java $J/RomSlice.java $J/VmEcid.java $J/VmRunnerInstalledLauncher.java tests/java/G7CloneTest.java tests/java/dev/muns/hengeprobe/BuildConfig.java 2> "$O8/javac.log" || { echo "FAILED javac (clone)"; head -30 "$O8/javac.log"; exit 1; }
java -cp "$O8" dev.muns.hengeprobe.G7CloneTest; R8=$?
# the clone-boot launcher (fake process) and the boot-stage estimate
O9="$OUT-cboot"; rm -rf "$O9"; mkdir -p "$O9"
javac -encoding UTF-8 -d "$O9" $X/G7ExecLauncher.java $X/G7Isolation.java $X/G7Storage.java $X/G7Clone.java $X/G7CloneProgress.java $X/G7CloneBoot.java $X/G7ExecDumpExport.java $X/G7BootStage.java $X/G7CloneMonitor.java $X/G7RunVerdict.java $X/G7RestartPolicy.java tests/java/G7CloneMonitorTest.java tests/java/G7RestartPolicyTest.java $J/G7ProbeLauncher.java $J/VmRomCheck.java $J/RomSlice.java $J/VmEcid.java $J/VmRunnerInstalledLauncher.java tests/java/G7CloneTest.java tests/java/G7CloneBootTest.java tests/java/G7BootStageTest.java tests/java/dev/muns/hengeprobe/BuildConfig.java 2> "$O9/javac.log" || { echo "FAILED javac (cloneboot)"; head -30 "$O9/javac.log"; exit 1; }
java -cp "$O9" dev.muns.hengeprobe.G7CloneBootTest; R9=$?
java -cp "$O9" dev.muns.hengeprobe.G7BootStageTest; R10=$?
javac -encoding UTF-8 -d "$O9" $X/G7AuxCompare.java $X/G7ExecLauncher.java $X/G7Isolation.java $X/G7Storage.java $X/G7Clone.java $X/G7CloneProgress.java $J/G7ProbeLauncher.java $J/VmRomCheck.java $J/RomSlice.java $J/VmEcid.java $J/VmRunnerInstalledLauncher.java tests/java/G7AuxCompareTest.java tests/java/dev/muns/hengeprobe/BuildConfig.java 2>> "$O9/javac.log" || { echo "FAILED javac (auxcompare)"; head -30 "$O9/javac.log"; exit 1; }
java -cp "$O9" dev.muns.hengeprobe.G7AuxCompareTest || R10=1
javac -encoding UTF-8 -d "$O9" $X/G7CloneHistory.java $X/G7ExecLauncher.java $X/G7Isolation.java $X/G7Storage.java $X/G7Clone.java $X/G7CloneProgress.java $J/G7ProbeLauncher.java $J/VmRomCheck.java $J/RomSlice.java $J/VmEcid.java $J/VmRunnerInstalledLauncher.java tests/java/G7CloneHistoryTest.java tests/java/dev/muns/hengeprobe/BuildConfig.java 2>> "$O9/javac.log" || { echo "FAILED javac (clonehistory)"; head -30 "$O9/javac.log"; exit 1; }
java -cp "$O9" dev.muns.hengeprobe.G7CloneHistoryTest || R10=1
java -cp "$O9" dev.muns.hengeprobe.G7CloneMonitorTest || R10=1
java -cp "$O9" dev.muns.hengeprobe.G7RestartPolicyTest || R10=1
javac -encoding UTF-8 -d "$O9" $X/G7ExecDumpExport.java $X/G7ExecLauncher.java $X/G7Isolation.java $X/G7Storage.java $X/G7Clone.java $X/G7CloneProgress.java $J/G7ProbeLauncher.java $J/VmRomCheck.java $J/RomSlice.java $J/VmEcid.java $J/VmRunnerInstalledLauncher.java tests/java/G7ExecDumpExportTest.java tests/java/dev/muns/hengeprobe/BuildConfig.java 2>> "$O9/javac.log" || { echo "FAILED javac (dumpexport)"; head -30 "$O9/javac.log"; exit 1; }
java -cp "$O9" dev.muns.hengeprobe.G7ExecDumpExportTest || R10=1
[ $R1 = 0 ] && [ $R2 = 0 ] && [ $R3 = 0 ] && [ $R4 = 0 ] && [ $R5 = 0 ] && [ $R6 = 0 ] && [ $R7 = 0 ] && [ $R8 = 0 ] && [ $R9 = 0 ] && [ $R10 = 0 ]
