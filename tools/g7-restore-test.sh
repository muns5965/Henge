#!/bin/bash
# host/g7_restore.c (the restore adapter and the judgment of its result) with a fake operations table. No restore tool, no machine, no network. Plain and ASan+UBSan.
# Usage: [CC=cc] tools/g7-restore-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}; mkdir -p "$OUT"; rc=0
ROMSHA=$(python3 -c "import hashlib; print(hashlib.sha256(bytes((i*31+7)&255 for i in range(214904))).hexdigest())")
RD="-DHV_G7_ROM_SHA256_HEX=\"$ROMSHA\""
for mode in plain asan; do
  case $mode in plain) F="-O1";; asan) F="-O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined";; esac
  echo "=== g7 restore $mode"
  $CC -std=gnu11 -g -Wall -Wextra -Werror -Wno-unused-parameter -c -Icore -Ihost -Idevices host/g7_restore.c -o "$OUT/g7r_obj_$mode.o" 2> "$OUT/g7r_obj_$mode.log" \
    || { echo "FAILED strict build of host/g7_restore.c"; head -30 "$OUT/g7r_obj_$mode.log"; rc=1; continue; }
  $CC -std=gnu11 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-result -Wno-format-truncation $F -DHV_G7_MIN_WRITTEN_BYTES='(2ULL<<20)' -DHV_G7_MIN_FREE_BYTES='(1ULL<<20)' $RD -Icore -Ihost -Idevices \
    tests/test_g7_restore.c host/g7_restore.c host/g7_preflight.c host/disk_policy.c host/crypto_portable.c host/disk.c -o "$OUT/g7r_$mode" -lm -lpthread 2> "$OUT/g7r_$mode.build.log" \
    || { echo "FAILED build ($mode)"; head -30 "$OUT/g7r_$mode.build.log"; rc=1; continue; }
  "$OUT/g7r_$mode" > "$OUT/g7r_$mode.out" 2>&1 || rc=1
  grep -E "^FAIL|FAILED" "$OUT/g7r_$mode.out"
  tail -1 "$OUT/g7r_$mode.out"
done
exit $rc
