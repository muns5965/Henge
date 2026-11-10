#!/bin/sh
# Builds libzip (cmake, host), the HTTP client stand-in (host/curl_mbedtls.c), host/remote_zip.c and tests/remote_zip_tool.c, then runs tests/test_remote_zip.py
# against local Range servers (a 5 GiB virtual ZIP64). Nothing leaves the machine. Usage: [CC=..] tools/remote-zip-test.sh [outdir]
set -e
cd "$(dirname "$0")/.."
O=${1:-build-rz}; CC=${CC:-cc}; TP=third_party
CURLTOOL_CFLAGS=-DHV_ALLOW_CA_ENV tools/curltool-build.sh "$O" > /dev/null
ZB="$O/libzip-build"
if [ ! -f "$ZB/lib/libzip.a" ]; then
  cmake -S $TP/libzip -B "$ZB" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DENABLE_COMMONCRYPTO=OFF -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF -DENABLE_OPENSSL=OFF \
    -DENABLE_WINDOWS_CRYPTO=OFF -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF \
    -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF -DLIBZIP_DO_INSTALL=OFF > "$O/libzip.cfg.log" 2>&1
  cmake --build "$ZB" -j4 > "$O/libzip.build.log" 2>&1
fi
$CC -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 -D_GNU_SOURCE -DHV_RZ_ALLOW_HTTP_TEST -Ihost -I$TP/curl_include -I$ZB -I$TP/libzip/lib -c host/remote_zip.c -o "$O/remote_zip.o"
$CC -O2 -Wall -std=gnu11 -Ihost -c tests/remote_zip_tool.c -o "$O/remote_zip_tool.o"
$CC -o "$O/remote_zip_tool" "$O/remote_zip_tool.o" "$O/remote_zip.o" "$O/curl_mbedtls.o" $O/obj/*.o "$ZB/lib/libzip.a" -lz -lpthread -lm
echo "linked  $O/remote_zip_tool"
python3 tests/test_remote_zip.py "$O/remote_zip_tool" "$O/work"
