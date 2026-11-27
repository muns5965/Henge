#!/bin/bash
# host/g7_fb0.h (early framebuffer observation, g7-fb0: lines) against synthetic buffers (tests/test_g7_fb0.c). No VM is run. Usage: [CC=cc] tools/g7-fb0-test.sh [build dir]
set -u
cd "$(dirname "$0")/.."
OUT=${1:-$(mktemp -d)}; CC=${CC:-cc}; mkdir -p "$OUT"
$CC -std=gnu11 -Wall -Wextra -Werror -O2 -g tests/test_g7_fb0.c -o "$OUT/test_g7_fb0" 2> "$OUT/build.log" || { echo "FAILED  build test_g7_fb0"; head -20 "$OUT/build.log"; exit 1; }
"$OUT/test_g7_fb0" | tee "$OUT/run.log" | grep -E "FAIL|G7FB0|INFO"
grep -q "^G7FB0 OK" "$OUT/run.log"
