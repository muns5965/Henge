#!/bin/sh
# Stop B: host/tss_stage.c (classification), the curl stand-in's per-request records and size caps, and the connection policy's test-only loopback, with the real
# libtatsu against a fake TSS server on this machine (tests/test_tss_stage.py). Nothing leaves the machine. Linux.
# Usage: [CC=..] tools/tss-stage-test.sh [outdir]
set -e
cd "$(dirname "$0")/.."
O=${1:-build-tssstage}
CC=${CC:-cc}
TP=third_party
CURLTOOL_CFLAGS=-DHV_ALLOW_CA_ENV tools/curltool-build.sh "$O" > /dev/null
mkdir -p "$O/tatsu_plist" "$O/tatsu_test" "$O/stage"
FLAGS="-O2 -w -DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/curl_include -I$TP/libplist/include -I$TP/libplist/libcnary/include -I$TP/libtatsu/include -I$TP/mbedtls/include"
for f in $TP/libplist/src/*.c $TP/libplist/libcnary/node.c $TP/libplist/libcnary/node_list.c; do
  o="$O/tatsu_plist/$(basename "$f" .c).o"
  [ "$o" -nt "$f" ] || $CC $FLAGS -I"$(dirname "$f")" -DPACKAGE_VERSION=\"2.8.0-henge\" -c "$f" -o "$o"
done
for f in $TP/libtatsu/src/*.c; do
  b=$(basename "$f" .c)
  $CC $FLAGS -I$TP/libtatsu/src -DPACKAGE_VERSION=\"1.0.5-henge\" -DHV_TSS_HTTPS_ONLY -DHV_TSS_ALLOW_HTTP_TEST -c "$f" -o "$O/tatsu_test/t_$b.o"
done
SF="-O1 -g -w -std=gnu11 -D_GNU_SOURCE -Ihost -I$TP/curl_include -I$TP/libplist/include -I$TP/libtatsu/include -I$TP/libimobiledevice-glue/include"
$CC $SF -DHV_NET_TEST_LOOPBACK -c host/net_guard.c -o "$O/stage/net_guard.o"
$CC $SF -DHV_TSS_HTTPS_ONLY -c host/tss_stage.c -o "$O/stage/tss_stage.o"
$CC $SF -c tests/tss_stage_tool.c -o "$O/stage/tool.o"
$CC -o "$O/tss_stage_tool" "$O/stage/tool.o" "$O/stage/tss_stage.o" "$O/stage/net_guard.o" "$O/tatsu_test"/*.o $O/tatsu_plist/*.o "$O/curl_mbedtls.o" $O/obj/*.o \
  -Wl,--wrap=connect,--wrap=getaddrinfo -lpthread -lm
echo "linked  $O/tss_stage_tool"
python3 tests/test_tss_stage.py "$O/tss_stage_tool" "$O/work"
