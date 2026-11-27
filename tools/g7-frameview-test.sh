#!/bin/bash
# integ7b T9: the full-resolution boot-screen picture. A synthetic 1920x1080 BGRA frame goes through the real shared-file writer (host/frame_shm.c), the runner's file writer (host/g7_frame_file.h) and the app's
# Java reader (G7FrameFile), plus the coordinate mapping (G7InputMap), torn-read protection and the FRAMEFILE line. C plain and ASan/UBSan, then the JVM test. Any Linux.
# Usage: tools/g7-frameview-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}; X=android/app/src/g7exec/java/dev/muns/hengeprobe
rm -rf "$OUT"; mkdir -p "$OUT"
rc=0
for mode in plain asan; do
  case $mode in plain) F="-O2";; asan) F="-O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined";; esac
  mkdir -p "$OUT/$mode"
  $CC -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -g $F -Ihost tests/test_g7_framefile.c host/frame_shm.c -o "$OUT/test_g7_framefile_$mode" 2> "$OUT/cc_$mode.log" || { echo "FAILED  build ($mode)"; head -30 "$OUT/cc_$mode.log"; rc=1; continue; }
  "$OUT/test_g7_framefile_$mode" "$OUT/$mode" || { echo "FAILED  C part ($mode)"; rc=1; continue; }
  javac -encoding UTF-8 -d "$OUT/classes_$mode" $X/G7FrameFile.java $X/G7InputMap.java tests/java/G7FrameFileTest.java 2> "$OUT/javac_$mode.log" || { echo "FAILED javac"; grep -v "Picked up" "$OUT/javac_$mode.log" | head -30; rc=1; continue; }
  java -cp "$OUT/classes_$mode" dev.muns.hengeprobe.G7FrameFileTest "$OUT/$mode" 2>&1 | grep -v "Picked up" > "$OUT/java_$mode.out"; [ "${PIPESTATUS[0]}" = 0 ] || rc=1
  grep -v "^PASS" "$OUT/java_$mode.out"; echo "($mode: $(grep -c '^PASS' "$OUT/java_$mode.out") checks passed)"
done
exit $rc
