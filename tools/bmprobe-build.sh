#!/bin/sh
# Builds tests/bm_probe.c (host/remote_zip.c + libzip + libplist + host/curl_mbedtls.c) into OUT/bm_probe. Native by default; for Android set CC to the NDK clang and
# NDK_ROOT (libzip is then built with the NDK toolchain file) and CURLTOOL_LIBS="". No HV_*_TEST switch: https only. Usage: [CC=..] [NDK_ROOT=..] tools/bmprobe-build.sh OUT
set -e
cd "$(dirname "$0")/.."
O=${1:?output dir}; CC=${CC:-cc}; TP=third_party
tools/curltool-build.sh "$O" > /dev/null          # mbedtls objects + curl_mbedtls.o (no HV_ALLOW_CA_ENV)
ZB="$O/libzip-build"
if [ ! -f "$ZB/lib/libzip.a" ]; then
  TC=""; [ -n "${NDK_ROOT:-}" ] && TC="-DCMAKE_TOOLCHAIN_FILE=$NDK_ROOT/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30"
  cmake -S $TP/libzip -B "$ZB" $TC -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DENABLE_COMMONCRYPTO=OFF -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF -DENABLE_OPENSSL=OFF \
    -DENABLE_WINDOWS_CRYPTO=OFF -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF \
    -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF -DLIBZIP_DO_INSTALL=OFF > "$O/libzip.cfg.log" 2>&1
  cmake --build "$ZB" -j4 > "$O/libzip.build.log" 2>&1
fi
mkdir -p "$O/bmp"
FL="-O2 -w -DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/libplist/include -I$TP/libplist/libcnary/include"
for f in $TP/libplist/src/*.c $TP/libplist/libcnary/node.c $TP/libplist/libcnary/node_list.c; do
  $CC $FL -I"$(dirname "$f")" -DPACKAGE_VERSION=\"2.8.0-henge\" -c "$f" -o "$O/bmp/$(basename "$f" .c).o"
done
$CC -O2 -Wall -std=gnu11 -D_GNU_SOURCE -Ihost -I$TP/curl_include -I$ZB -I$TP/libzip/lib -c host/remote_zip.c -o "$O/bmp/remote_zip.o"
$CC -O2 -Wall -std=gnu11 -Ihost -I$TP/libplist/include -c tests/bm_probe.c -o "$O/bmp/bm_probe.o"
$CC -o "$O/bm_probe" $O/bmp/*.o "$O/curl_mbedtls.o" $O/obj/*.o "$ZB/lib/libzip.a" -lz ${CURLTOOL_LIBS--lpthread} -lm
echo "linked  $O/bm_probe"
