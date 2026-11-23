#!/bin/bash
# The G7 network allow-list (host/install_g7.c) against host/net_guard.c (tests/test_g7_net.c) + source-order checks on host/install.c. Linux.
# Usage: [CC=cc] tools/g7-net-test.sh <build dir>
set -u
OUT=${1:?build dir}
CC=${CC:-cc}
TP=third_party
[ -f "$OUT/rs/lib.a" ] || tools/restore-selftest-build.sh "$OUT" > "$OUT.build.log" 2>&1 || { echo "FAILED  restore stack build"; tail -20 "$OUT.build.log"; exit 1; }
R="$OUT/rs"; mkdir -p "$OUT/g7n"
FL="-std=gnu11 -D_GNU_SOURCE -O1 -g -w -I$TP/curl_include -I$TP/libimobiledevice-glue/include -Ihost"
$CC $FL tests/test_g7_net.c host/install_g7.c host/net_guard.c "$R/lib.a" -lz -lm -lpthread -o "$OUT/g7n/test_g7_net" 2> "$OUT/g7n/link.log" || { echo "FAILED  build test_g7_net"; head -20 "$OUT/g7n/link.log"; exit 1; }
"$OUT/g7n/test_g7_net" || exit 1
# integration with the REAL restore session + lifetime registry (stand-in engine; no device, no network)
$CC -std=gnu11 -D_GNU_SOURCE -O1 -g -w -I$TP/idevicerestore/src -I$TP/libimobiledevice/src -I$TP/libplist/include -I$TP -I. -Icore -Idevices -Ihost tests/test_g7_net_session.c host/install_g7.c tests/fake_device/stubs.c "$R/lib.a" "$R/libzip-build/lib/libzip.a" -lz -lm -lpthread -o "$OUT/g7n/test_g7_net_session" 2> "$OUT/g7n/link2.log" || { echo "FAILED  build test_g7_net_session"; head -20 "$OUT/g7n/link2.log"; exit 1; }
timeout 120 "$OUT/g7n/test_g7_net_session" || exit 1
fail=0
chk() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; fail=$((fail+1)); fi; }
ln() { grep -n "$1" host/install.c | head -1 | cut -d: -f1; }
b=$(ln 'hv_restore_session_begin('); pc=$(ln 'pthread_create(.*restore_thread'); cl=$(ln 'hv_restore_session_claim('); hk=$(ln 'hv_g7_extra_flags()'); rc=$(ln 'hv_restore_session_run_client(')
for v in b pc cl hk rc; do [ -n "${!v}" ] || { echo "FAIL install.c: anchor $v missing"; exit 1; }; done
[ "$b" -lt "$pc" ] && chk "session begin precedes the restore thread" ok || chk "session begin precedes the restore thread" bad
[ "$cl" -lt "$hk" ] && [ "$hk" -lt "$rc" ] && chk "hook sits after session claim and before the tool is entered" ok || chk "hook order" bad
[ "$(grep -c 'hv_g7_extra_flags' host/install.c)" = 1 ] && grep -B1 'hv_g7_extra_flags' host/install.c | grep -q 'ifdef HV_INSTALL_G7' && chk "hook is compiled only under HV_INSTALL_G7" ok || chk "hook ifdef" bad
gt=$(ln 'hv_g7_net_gate()'); [ -n "$gt" ] && [ "$hk" -lt "$gt" ] && [ "$gt" -lt "$rc" ] && chk "the gate sits between the registration and the tool entry" ok || chk "gate order" bad
grep -q 'g7_blocked' host/install.c && grep -B4 'hv_restore_session_run_client(c' host/install.c | grep -q 'if (g7_blocked)' && grep -q 'idevicerestore_start(c)' host/install.c && grep 'idevicerestore_start(c)' host/install.c | grep -q g7_blocked && chk "both entry paths (session and plain) are skipped when blocked" ok || chk "both entry paths" bad
grep -q 'hv_net_guard_reset' host/restore_session.c && chk "session begin is what resets the lists (why the order matters)" ok || chk "reset in session" bad
[ "$fail" = 0 ] && echo "G7NET SOURCE OK" || { echo "G7NET SOURCE FAILED"; exit 1; }
