#!/bin/bash
# The G7 runner's per-attempt TSS line (host/g7_tssdiag.h) against synthetic values (tests/test_g7_tssdiag.c). Nothing is run on a device, nothing is connected. Usage: [CC=cc] tools/g7-tssdiag-test.sh [build dir]
set -u
cd "$(dirname "$0")/.."
OUT=${1:-$(mktemp -d)}; CC=${CC:-cc}; mkdir -p "$OUT"
$CC -std=gnu11 -Wall -Wextra -Werror -O1 -g tests/test_g7_tssdiag.c -o "$OUT/test_g7_tssdiag" 2> "$OUT/build.log" || { echo "FAILED  build test_g7_tssdiag"; head -20 "$OUT/build.log"; exit 1; }
"$OUT/test_g7_tssdiag" | tee "$OUT/run.log" | grep -E "FAIL|G7TSSDIAG"
grep -q "^G7TSSDIAG OK" "$OUT/run.log"
