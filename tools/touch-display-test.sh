#!/bin/bash
# The touch target follows the guest display's real size (hv_touch_follow_display; android/app/src/main/cpp/ui_jni.c calls it with the size of each new picture in
# runner mode). tests/test_touch_display.c over host/touch_input.c on any Linux, plain and ASan+UBSan; plus a source check that ui_jni.c really calls it
# after hv_dispbuf_size. Usage: [CC=cc] tools/touch-display-test.sh <build dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?build dir}; CC=${CC:-cc}; mkdir -p "$OUT"; rc=0
for mode in plain asan; do
  case $mode in plain) F="-O1";; asan) F="-O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined";; esac
  $CC -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -g $F tests/test_touch_display.c host/touch_input.c -o "$OUT/td_$mode" -lm 2> "$OUT/td_$mode.log" || { echo "FAILED build ($mode)"; head -20 "$OUT/td_$mode.log"; rc=1; continue; }
  "$OUT/td_$mode" | sed "s/^/[$mode] /" | grep -E "FAIL|test_touch_display|PASS" ; "$OUT/td_$mode" > /dev/null || rc=1
done
if awk '/hv_dispbuf_size\(b, &fw, &fh\)/{a=1} a && /hv_touch_follow_display\(&g_touch/{ok=1} END{exit !ok}' android/app/src/main/cpp/ui_jni.c; then echo "PASS ui_jni.c follows the picture size"; else echo "FAIL ui_jni.c does not call hv_touch_follow_display after hv_dispbuf_size"; rc=1; fi
[ $rc = 0 ] && echo "TOUCH DISPLAY TEST OK" || echo "TOUCH DISPLAY TEST FAILED"
exit $rc
