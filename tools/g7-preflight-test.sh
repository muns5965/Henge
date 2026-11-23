#!/bin/bash
# host/g7_preflight.c with a mock writer in temporary directories (tests/test_g7_preflight.c). Linux, no engine, no network. Plain and ASan+UBSan.
# Usage: [CC=cc] tools/g7-preflight-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}; mkdir -p "$OUT"; rc=0
ROMSHA=$(python3 -c "import hashlib; print(hashlib.sha256(bytes((i*31+7)&255 for i in range(214904))).hexdigest())")
RD="-DHV_G7_ROM_SHA256_HEX=\"$ROMSHA\""
for mode in plain asan; do
  case $mode in plain) F="-O1";; asan) F="-O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined";; esac
  echo "=== g7 preflight $mode"
  $CC -std=gnu11 -g -Wall -Wextra -Werror -Wno-unused-parameter -c -Icore -Ihost -Idevices host/g7_preflight.c -o "$OUT/g7_obj_$mode.o" 2> "$OUT/g7_obj_$mode.log" \
     || { echo "FAILED strict build of host/g7_preflight.c"; head -30 "$OUT/g7_obj_$mode.log"; rc=1; continue; }
  $CC -std=gnu11 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-result -Wno-format-truncation $F -DHV_G7_MIN_FREE_BYTES='(1ULL<<20)' $RD -Icore -Ihost -Idevices \
     tests/test_g7_preflight.c host/g7_preflight.c host/disk_policy.c host/crypto_portable.c host/disk.c host/runner_vm_core.c host/frame_shm.c host/display_buf.c -o "$OUT/g7_$mode" -lm -lpthread 2> "$OUT/g7_$mode.build.log" \
     || { echo "FAILED build ($mode)"; head -30 "$OUT/g7_$mode.build.log"; rc=1; continue; }
  for variant in ship mock; do
    VD=""; [ $variant = mock ] && VD="-DHV_G7_MOCK_EXECUTION"
    echo "=== g7 runner skeleton $mode ($variant)"
    $CC -std=gnu11 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-result -Wno-format-truncation $F $VD -DHV_G7_MIN_FREE_BYTES='(1ULL<<20)' $RD -Icore -Ihost -Idevices \
       tests/test_g7_runner.c host/runner_g7_core.c host/g7_preflight.c host/disk_policy.c host/crypto_portable.c host/disk.c host/runner_vm_core.c host/frame_shm.c host/display_buf.c -o "$OUT/g7r_${mode}_$variant" -lm -lpthread 2> "$OUT/g7r_${mode}_$variant.build.log" \
       || { echo "FAILED build (runner $mode $variant)"; head -30 "$OUT/g7r_${mode}_$variant.build.log"; rc=1; continue; }
    "$OUT/g7r_${mode}_$variant" > "$OUT/g7r_${mode}_$variant.out" 2> "$OUT/g7r_${mode}_$variant.err" || { echo "FAILED run (runner $mode $variant)"; grep -E "^FAIL" "$OUT/g7r_${mode}_$variant.out" | head -30; head -20 "$OUT/g7r_${mode}_$variant.err"; rc=1; continue; }
    grep -c '^PASS' "$OUT/g7r_${mode}_$variant.out" | sed 's/^/PASS lines: /'; tail -1 "$OUT/g7r_${mode}_$variant.out"
    [ -s "$OUT/g7r_${mode}_$variant.err" ] && { echo "unexpected stderr (runner $mode $variant)"; head -10 "$OUT/g7r_${mode}_$variant.err"; rc=1; }
  done
  echo "=== g7 preflight $mode (module tests)"
  "$OUT/g7_$mode" > "$OUT/g7_$mode.out" 2> "$OUT/g7_$mode.err" || { echo "FAILED run ($mode)"; grep -E "^FAIL" "$OUT/g7_$mode.out" | head -30; head -20 "$OUT/g7_$mode.err"; rc=1; continue; }
  grep -c '^PASS' "$OUT/g7_$mode.out" | sed 's/^/PASS lines: /'; tail -1 "$OUT/g7_$mode.out"
  [ -s "$OUT/g7_$mode.err" ] && { echo "unexpected stderr ($mode)"; head -10 "$OUT/g7_$mode.err"; rc=1; }
done
exit $rc
