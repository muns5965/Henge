#!/bin/bash
# tests/test_registry.c: host/restore_registry.c's contracts, one case per process. Linux; no restore stack needed.
# Usage: [CC=cc] tools/registry-test.sh <build dir> [sanitizer: none|asan|tsan]
set -u
OUT=${1:?build dir}; SAN=${2:-none}; CC=${CC:-cc}
case $SAN in asan) SF="-fsanitize=address -fno-omit-frame-pointer";; tsan) SF="-fsanitize=thread";; *) SF="";; esac
mkdir -p "$OUT"; rc=0
for flavor in plain hooks; do
  D=""; CASES="qf normal errors storm cancel phase conn watchdog"
  [ $flavor = hooks ] && { D="-DHV_TEST_HOOKS"; CASES="$CASES hooks"; }
  $CC -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra $SF $D -Ihost tests/test_registry.c host/restore_registry.c -lpthread -o "$OUT/test_registry_$flavor" || { echo "FAILED build $flavor"; exit 1; }
  for c in $CASES; do
    timeout 60 "$OUT/test_registry_$flavor" $c > "$OUT/$flavor.$c.out" 2> "$OUT/$flavor.$c.err"; r=$?
    f=$(grep -c "^FAIL" "$OUT/$flavor.$c.out"); p=$(grep -c "^PASS" "$OUT/$flavor.$c.out")
    s=$(grep -c "ThreadSanitizer\|AddressSanitizer" "$OUT/$flavor.$c.err")
    echo "$flavor/$c: exit $r, $p pass, $f fail, sanitizer reports $s"
    [ $r = 0 ] && [ $f = 0 ] && [ $s = 0 ] && [ $p -gt 0 ] || { rc=1; grep "^FAIL" "$OUT/$flavor.$c.out"; head -5 "$OUT/$flavor.$c.err"; }
  done
done
[ $rc = 0 ] && echo "REGISTRY OK" || echo "REGISTRY FAILED"
exit $rc
