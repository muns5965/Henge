#!/bin/bash
# tests/test_use_tag.c against the restore stack in <build dir> (build it with CC="cc -fsanitize=thread -g" for the race report). Linux.
# Usage: [CC=cc] tools/use-tag-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}; TP=third_party; R="$OUT/rs"
[ -f "$R/lib.a" ] || tools/restore-selftest-build.sh "$OUT" > "$OUT.build.log" 2>&1 || { echo "FAILED restore stack build"; exit 1; }
mkdir -p "$OUT/ut"
$CC -std=gnu11 -D_GNU_SOURCE -O1 -g -w -I$TP/libusbmuxd/include -I$TP/libplist/include -I$TP/libimobiledevice-glue/include tests/test_use_tag.c "$R/lib.a" -lm -lpthread -o "$OUT/ut/t" 2> "$OUT/ut/link.log" || { echo "FAILED build"; head -20 "$OUT/ut/link.log"; exit 1; }
TSAN_OPTIONS="halt_on_error=0" timeout 120 "$OUT/ut/t" ${THREADS:-8} ${CALLS:-100} > "$OUT/ut/out" 2> "$OUT/ut/err"; echo "exit=$?"
cat "$OUT/ut/out"; grep -E "WARNING: ThreadSanitizer|SUMMARY:" "$OUT/ut/err" | sort | uniq -c | head -5
