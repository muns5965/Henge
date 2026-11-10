#!/bin/sh
# Builds tests/curl_tool.c with host/curl_mbedtls.c and the vendored mbedtls into OUT/curl_tool.
# Usage: [CC=..] [CURLTOOL_CFLAGS=..] [CURLTOOL_LIBS=..] tools/curltool-build.sh OUT
# The CI test passes CURLTOOL_CFLAGS=-DHV_ALLOW_CA_ENV; the copy shipped in the APK is built without it.
set -e
cd "$(dirname "$0")/.."
O=${1:?output dir}; mkdir -p "$O/obj"
TP=third_party
tools/check-apple-ca.sh
CC=${CC:-cc}
n=0
for f in $TP/mbedtls/library/*.c; do
  o="$O/obj/$(basename "$f" .c).o"
  if [ ! "$o" -nt "$f" ]; then
    $CC -O2 -w -I$TP/mbedtls/include -I$TP/mbedtls/library -c "$f" -o "$o" &
    n=$((n+1)); [ $((n % 8)) = 0 ] && wait
  fi
done
wait
$CC -O2 -Wall -Wno-format-truncation ${CURLTOOL_CFLAGS:-} -Ihost -I$TP/curl_include -I$TP/mbedtls/include -c host/curl_mbedtls.c -o "$O/curl_mbedtls.o"
$CC -O2 -Wall -I$TP/curl_include -c tests/curl_tool.c -o "$O/curl_tool.o"
$CC -o "$O/curl_tool" "$O/curl_tool.o" "$O/curl_mbedtls.o" $O/obj/*.o ${CURLTOOL_LIBS--lpthread}
echo "linked  $O/curl_tool"
