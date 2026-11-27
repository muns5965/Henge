#!/bin/bash
# host/disk.c counters (tests/test_disk_stats.c) in a temporary directory. Linux, any compiler.
# Usage: [CC=cc] tools/disk-stats-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}; mkdir -p "$OUT"
$CC -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-result -Icore -Ihost -Idevices tests/test_disk_stats.c host/disk.c -o "$OUT/test_disk_stats" 2> "$OUT/disk_stats.log" || { echo "FAILED  build"; head -20 "$OUT/disk_stats.log"; exit 1; }
"$OUT/test_disk_stats"
