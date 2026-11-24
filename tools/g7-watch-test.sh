#!/bin/bash
# The G7 runner's watchdog line logic (host/g7_watch.h) against synthetic samples (tests/test_g7_watch.c). Nothing is run on a device, nothing is connected. Usage: [CC=cc] tools/g7-watch-test.sh [build dir]
set -u
cd "$(dirname "$0")/.."
OUT=${1:-$(mktemp -d)}; CC=${CC:-cc}; mkdir -p "$OUT"
$CC -std=gnu11 -Wall -Wextra -Werror -O1 -g tests/test_g7_watch.c -o "$OUT/test_g7_watch" 2> "$OUT/build.log" || { echo "FAILED  build test_g7_watch"; head -20 "$OUT/build.log"; exit 1; }
"$OUT/test_g7_watch" | tee "$OUT/run.log" | grep -E "FAIL|G7WATCH"
grep -q "^G7WATCH OK" "$OUT/run.log"
