#!/bin/sh
# Builds the real libtatsu + libplist with host/curl_mbedtls.c and runs tests/test_tatsu.py against mock TSS servers.
# Usage: [CC=..] tools/tatsu-test.sh [outdir]   (reuses the mbedtls objects of tools/curltool-build.sh in the same outdir)
set -e
cd "$(dirname "$0")/.."
O=${1:-build-curltest}
CC=${CC:-cc}
TP=third_party
CURLTOOL_CFLAGS=-DHV_ALLOW_CA_ENV tools/curltool-build.sh "$O" > /dev/null
mkdir -p "$O/tatsu_plist"   # own directory: an older run (or files committed by mistake under build-curltest/tatsu) must not leak into the link
FLAGS="-O2 -w -DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/curl_include -I$TP/libplist/include -I$TP/libplist/libcnary/include -I$TP/libtatsu/include -I$TP/mbedtls/include"
for f in $TP/libplist/src/*.c $TP/libplist/libcnary/node.c $TP/libplist/libcnary/node_list.c; do
  $CC $FLAGS -I"$(dirname "$f")" -DPACKAGE_VERSION=\"2.8.0-henge\" -c "$f" -o "$O/tatsu_plist/$(basename "$f" .c).o"
done
# Two builds of libtatsu: the one the Android app ships (HV_TSS_HTTPS_ONLY) and the test one (the same plus HV_TSS_ALLOW_HTTP_TEST: loopback http only).
# Both link the same objects otherwise, so the shipped flavour is tested for real, not assumed.
mkdir -p "$O/tatsu_ship" "$O/tatsu_test"
for f in $TP/libtatsu/src/*.c; do
  b=$(basename "$f" .c)
  $CC $FLAGS -I$TP/libtatsu/src -DPACKAGE_VERSION=\"1.0.5-henge\" -DHV_TSS_HTTPS_ONLY -c "$f" -o "$O/tatsu_ship/t_$b.o"
  $CC $FLAGS -I$TP/libtatsu/src -DPACKAGE_VERSION=\"1.0.5-henge\" -DHV_TSS_HTTPS_ONLY -DHV_TSS_ALLOW_HTTP_TEST -c "$f" -o "$O/tatsu_test/t_$b.o"
done
$CC $FLAGS -c tests/test_tatsu.c -o "$O/tatsu_plist/test_tatsu.o"
for v in ship test; do
  $CC -o "$O/test_tatsu_$v" $O/tatsu_plist/*.o "$O/tatsu_$v"/*.o "$O/curl_mbedtls.o" $O/obj/*.o -lpthread -lm
done
echo "linked  $O/test_tatsu_test  $O/test_tatsu_ship"
python3 tests/test_tatsu.py "$O/test_tatsu_test" "$O/work" "$O/test_tatsu_ship"
