#!/bin/sh
# Builds host/curl_mbedtls.c with the vendored mbedtls for the build host and runs tests/test_curl.py. Usage: tools/curl-test.sh [outdir]
set -e
cd "$(dirname "$0")/.."
O=${1:-build-curltest}; mkdir -p "$O/obj"
TP=third_party
CC=${CC:-cc}
for f in $TP/mbedtls/library/*.c; do
  o="$O/obj/$(basename "$f" .c).o"
  [ "$o" -nt "$f" ] || $CC -O2 -w -I$TP/mbedtls/include -I$TP/mbedtls/library -c "$f" -o "$o" &
  while [ "$(jobs -p | wc -l)" -ge 8 ]; do wait; done
done
wait
$CC -O2 -Wall -I$TP/curl_include -I$TP/mbedtls/include -c host/curl_mbedtls.c -o "$O/curl_mbedtls.o"
$CC -O2 -Wall -I$TP/curl_include -c tests/curl_tool.c -o "$O/curl_tool.o"
$CC -o "$O/curl_tool" "$O/curl_tool.o" "$O/curl_mbedtls.o" $O/obj/*.o -lpthread
python3 tests/test_curl.py "$O/curl_tool" "$O/work"
