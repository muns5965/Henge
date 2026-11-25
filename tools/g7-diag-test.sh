#!/bin/bash
# The G7 runner diagnostic lines (host/g7_diag.h) against synthetic procfs text (tests/test_g7_diag.c). Nothing is run on a device, nothing is connected. Usage: [CC=cc] tools/g7-diag-test.sh [build dir]
set -u
cd "$(dirname "$0")/.."
OUT=${1:-$(mktemp -d)}; CC=${CC:-cc}; mkdir -p "$OUT"
$CC -std=gnu11 -Wall -Wextra -Werror -O1 -g tests/test_g7_diag.c -o "$OUT/test_g7_diag" 2> "$OUT/build.log" || { echo "FAILED  build test_g7_diag"; head -20 "$OUT/build.log"; exit 1; }
"$OUT/test_g7_diag" | tee "$OUT/run.log" | grep -E "FAIL|G7DIAG"
grep -q "^G7DIAG OK" "$OUT/run.log"
