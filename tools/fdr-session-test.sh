#!/bin/bash
# tests/test_fdr_session.c: the FDR proxy and the URLAsset step against a fake device, and the restore session's rules. Linux. Needs the restore stack.
# Usage: [CC=cc] tools/fdr-session-test.sh <build dir>
set -u
OUT=${1:?build dir}
CC=${CC:-cc}
TP=third_party
[ -f "$OUT/rs/lib.a" ] || tools/restore-selftest-build.sh "$OUT" > "$OUT.build.log" 2>&1 || { echo "FAILED  restore stack build"; tail -20 "$OUT.build.log"; exit 1; }
R="$OUT/rs"; ZB="$R/libzip-build"
mkdir -p "$OUT/fs"
FL="-std=gnu11 -D_GNU_SOURCE -O1 -g -w -DHAVE_CONFIG_H -DHAVE_MBEDTLS -I$TP/restore_config -I$TP -I$TP/curl_include -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libusbmuxd/include -I$TP/libimobiledevice/include -I$TP/libimobiledevice/src -I$TP/libimobiledevice/common -I$TP/libimobiledevice -I$TP/libtatsu/include -I$TP/libirecovery/include -I$TP/mbedtls/include -I$TP/libzip/lib -I$ZB -I$TP/idevicerestore/src -Ihost -Idevices -Icore"
$CC $FL tests/test_fdr_session.c "$R/lib.a" "$ZB/lib/libzip.a" -lz -lm -lpthread -o "$OUT/fs/test_fdr_session" 2> "$OUT/fs/link.log" || { echo "FAILED  build test_fdr_session"; head -30 "$OUT/fs/link.log"; exit 1; }
timeout 300 "$OUT/fs/test_fdr_session" "${2:-all}"
