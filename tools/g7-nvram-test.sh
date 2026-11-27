#!/bin/bash
# host/g7_nvram.h (S0 aux-traffic log, S1 opt-in pre-seed) in temporary files (tests/test_g7_nvram.c). Linux, any compiler.
# Usage: [CC=cc] tools/g7-nvram-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}; mkdir -p "$OUT"
$CC -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-result -Icore -Ihost -Idevices tests/test_g7_nvram.c host/disk.c -lpthread -o "$OUT/test_g7_nvram" 2> "$OUT/g7_nvram.log" || { echo "FAILED  build"; head -30 "$OUT/g7_nvram.log"; exit 1; }
[ -s "$OUT/g7_nvram.log" ] && { echo "build warnings:"; head -20 "$OUT/g7_nvram.log"; }
"$OUT/test_g7_nvram"
