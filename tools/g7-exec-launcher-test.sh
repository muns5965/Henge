#!/bin/bash
# The app side of the G7 execution build (android/app/src/g7exec/.../G7ExecLauncher.java) on a plain JVM with fake processes: nothing is started, nothing real is touched. Usage: tools/g7-exec-launcher-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; J=android/app/src/main/java/dev/muns/hengeprobe; X=android/app/src/g7exec/java/dev/muns/hengeprobe
rm -rf "$OUT"; mkdir -p "$OUT"
javac -encoding UTF-8 -d "$OUT" $X/G7ExecLauncher.java $J/G7ProbeLauncher.java $J/VmRomCheck.java $J/RomSlice.java $J/VmEcid.java $J/VmRunnerInstalledLauncher.java tests/java/G7ExecLauncherTest.java 2> "$OUT/javac.log" || { echo "FAILED javac"; head -30 "$OUT/javac.log"; exit 1; }
java -cp "$OUT" dev.muns.hengeprobe.G7ExecLauncherTest
