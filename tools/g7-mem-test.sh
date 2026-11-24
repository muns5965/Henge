#!/bin/bash
# The G7 runner's memory line (host/g7_mem.h) against synthetic procfs text (tests/test_g7_mem.c). Nothing is run on a device, nothing is connected. Usage: [CC=cc] tools/g7-mem-test.sh [build dir]
set -u
cd "$(dirname "$0")/.."
OUT=${1:-$(mktemp -d)}; CC=${CC:-cc}; mkdir -p "$OUT"
$CC -std=gnu11 -Wall -Wextra -Werror -O1 -g tests/test_g7_mem.c -o "$OUT/test_g7_mem" 2> "$OUT/build.log" || { echo "FAILED  build test_g7_mem"; head -20 "$OUT/build.log"; exit 1; }
"$OUT/test_g7_mem" | tee "$OUT/run.log" | grep -E "FAIL|G7MEM"
grep -q "^G7MEM OK" "$OUT/run.log"
