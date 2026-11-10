#!/bin/bash
# host/disk_policy.c in a temporary directory (tests/test_disk_policy.c). Linux, any compiler.
# Usage: [CC=cc] tools/disk-policy-test.sh <build dir>
set -u
OUT=${1:?build dir}
CC=${CC:-cc}
mkdir -p "$OUT"
$CC -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Wno-unused-parameter -Ihost tests/test_disk_policy.c host/disk_policy.c -o "$OUT/test_disk_policy" 2> "$OUT/disk_policy.log" || { echo "FAILED  build"; head -20 "$OUT/disk_policy.log"; exit 1; }
"$OUT/test_disk_policy"
