#!/bin/bash
# host/g7_fbprobe.h (boot-framebuffer probe lines) against synthetic buffers (tests/test_g7_fbprobe.c). No VM is run. Usage: [CC=cc] tools/g7-fbprobe-test.sh [build dir]
set -u
cd "$(dirname "$0")/.."
OUT=${1:-$(mktemp -d)}; CC=${CC:-cc}; mkdir -p "$OUT"
$CC -std=gnu11 -Wall -Wextra -Werror -O1 -g tests/test_g7_fbprobe.c -o "$OUT/test_g7_fbprobe" 2> "$OUT/build.log" || { echo "FAILED  build test_g7_fbprobe"; head -20 "$OUT/build.log"; exit 1; }
"$OUT/test_g7_fbprobe" | tee "$OUT/run.log" | grep -E "FAIL|G7FBPROBE"
grep -q "^G7FBPROBE OK" "$OUT/run.log"
