#!/bin/bash
# host/net_guard.c against the real glue sockets and the curl stand-in (tests/test_netguard.c). Linux. Needs the restore stack (tools/restore-selftest-build.sh).
# Usage: [CC=cc] tools/netguard-test.sh <build dir>
set -u
OUT=${1:?build dir}
CC=${CC:-cc}
TP=third_party
[ -f "$OUT/rs/lib.a" ] || tools/restore-selftest-build.sh "$OUT" > "$OUT.build.log" 2>&1 || { echo "FAILED  restore stack build"; tail -20 "$OUT.build.log"; exit 1; }
R="$OUT/rs"
mkdir -p "$OUT/ng"
FL="-std=gnu11 -D_GNU_SOURCE -O1 -g -w -I$TP/curl_include -I$TP/libimobiledevice-glue/include -Ihost"
$CC $FL tests/test_netguard.c host/net_guard.c "$R/lib.a" -lz -lm -lpthread -o "$OUT/ng/test_netguard" 2> "$OUT/ng/link.log" || { echo "FAILED  build test_netguard"; head -20 "$OUT/ng/link.log"; exit 1; }
"$OUT/ng/test_netguard"
