#!/bin/sh
# Builds the real libtatsu + libplist with host/curl_mbedtls.c and runs tests/test_tatsu.py against mock TSS servers.
# Usage: [CC=..] tools/tatsu-test.sh [outdir]   (reuses the mbedtls objects of tools/curltool-build.sh in the same outdir)
set -e
cd "$(dirname "$0")/.."
O=${1:-build-curltest}
CC=${CC:-cc}
TP=third_party
CURLTOOL_CFLAGS=-DHV_ALLOW_CA_ENV tools/curltool-build.sh "$O" > /dev/null
mkdir -p "$O/tatsu"
FLAGS="-O2 -w -DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/curl_include -I$TP/libplist/include -I$TP/libplist/libcnary/include -I$TP/libtatsu/include -I$TP/mbedtls/include"
for f in $TP/libplist/src/*.c $TP/libplist/libcnary/node.c $TP/libplist/libcnary/node_list.c; do
  $CC $FLAGS -I"$(dirname "$f")" -DPACKAGE_VERSION=\"2.8.0-henge\" -c "$f" -o "$O/tatsu/$(basename "$f" .c).o"
done
for f in $TP/libtatsu/src/*.c; do
  $CC $FLAGS -I$TP/libtatsu/src -DPACKAGE_VERSION=\"1.0.5-henge\" -c "$f" -o "$O/tatsu/t_$(basename "$f" .c).o"
done
$CC $FLAGS -c tests/test_tatsu.c -o "$O/tatsu/test_tatsu.o"
$CC -o "$O/test_tatsu" $O/tatsu/*.o "$O/curl_mbedtls.o" $O/obj/*.o -lpthread -lm
echo "linked  $O/test_tatsu"
python3 tests/test_tatsu.py "$O/test_tatsu" "$O/work"
