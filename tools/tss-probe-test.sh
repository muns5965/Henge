#!/bin/sh
# Builds host/tss_probe_core.c (remote ZIP reader + libtatsu https-only + libplist + the HTTP client stand-in) with tests/test_tss_probe.c and runs
# tests/test_tss_probe.py against local mock IPSW and TSS servers. Nothing leaves the machine. Usage: [CC=..] tools/tss-probe-test.sh [outdir]
set -e
cd "$(dirname "$0")/.."
O=${1:-build-tssprobe}; CC=${CC:-cc}; TP=third_party
CURLTOOL_CFLAGS=-DHV_ALLOW_CA_ENV tools/curltool-build.sh "$O" > /dev/null
ZB="$O/libzip-build"
if [ ! -f "$ZB/lib/libzip.a" ]; then
  cmake -S $TP/libzip -B "$ZB" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DENABLE_COMMONCRYPTO=OFF -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF -DENABLE_OPENSSL=OFF \
    -DENABLE_WINDOWS_CRYPTO=OFF -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF \
    -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF -DLIBZIP_DO_INSTALL=OFF > "$O/libzip.cfg.log" 2>&1
  cmake --build "$ZB" -j4 > "$O/libzip.build.log" 2>&1
fi
mkdir -p "$O/tp"
FL="-O2 -w -DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/curl_include -I$TP/libplist/include -I$TP/libplist/libcnary/include -I$TP/libtatsu/include -I$TP/mbedtls/include"
for f in $TP/libplist/src/*.c $TP/libplist/libcnary/node.c $TP/libplist/libcnary/node_list.c; do
  $CC $FL -I"$(dirname "$f")" -DPACKAGE_VERSION=\"2.8.0-henge\" -c "$f" -o "$O/tp/$(basename "$f" .c).o"
done
for f in $TP/libtatsu/src/*.c; do
  $CC $FL -I$TP/libtatsu/src -DPACKAGE_VERSION=\"1.0.5-henge\" -DHV_TSS_HTTPS_ONLY -DHV_TSS_ALLOW_HTTP_TEST -c "$f" -o "$O/tp/t_$(basename "$f" .c).o"
done
$CC -O2 -Wall -std=gnu11 -D_GNU_SOURCE -DHV_RZ_ALLOW_HTTP_TEST -Ihost -I$TP/curl_include -I$ZB -I$TP/libzip/lib -c host/remote_zip.c -o "$O/tp/remote_zip.o"
$CC -O2 -Wall -std=gnu11 -D_GNU_SOURCE -DHV_TSS_HTTPS_ONLY -Ihost -I$TP/libplist/include -I$TP/libtatsu/include -c host/tss_probe_core.c -o "$O/tp/tss_probe_core.o"
$CC -O2 -Wall -std=gnu11 -Ihost -c tests/test_tss_probe.c -o "$O/tp/test_tss_probe.o"
$CC -o "$O/test_tss_probe" $O/tp/*.o "$O/curl_mbedtls.o" $O/obj/*.o "$ZB/lib/libzip.a" -lz -lpthread -lm
echo "linked  $O/test_tss_probe"
python3 tests/test_tss_probe.py "$O/test_tss_probe" "$O/work"
