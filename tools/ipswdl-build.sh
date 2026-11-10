#!/bin/sh
# Builds tests/ipsw_dl_tool.c (host/ipsw_dl.c + host/curl_mbedtls.c + the vendored mbedtls) into OUT/ipsw_dl. Native by default; for Android set CC to the NDK clang
# and CURLTOOL_LIBS="". The shipped build sets no HV_DL_* switch (https only, URL/size/SHA-256 compiled in, --stop-after required): tools/ipswdl-test.sh passes the
# test switches through DL_LIB_FLAGS / DL_TOOL_FLAGS, nothing else does. Usage: [CC=..] [DL_LIB_FLAGS=..] [DL_TOOL_FLAGS=..] tools/ipswdl-build.sh OUT
set -e
cd "$(dirname "$0")/.."
O=${1:?output dir}; CC=${CC:-cc}; TP=third_party
tools/curltool-build.sh "$O" > /dev/null          # mbedtls objects + curl_mbedtls.o (no HV_ALLOW_CA_ENV)
mkdir -p "$O/dl"
$CC -O2 -Wall -std=gnu11 -D_GNU_SOURCE ${DL_LIB_FLAGS:-} -Ihost -I$TP/curl_include -I$TP/mbedtls/include -c host/ipsw_dl.c -o "$O/dl/ipsw_dl.o"
$CC -O2 -Wall -std=gnu11 ${DL_TOOL_FLAGS:-} -Ihost -c tests/ipsw_dl_tool.c -o "$O/dl/ipsw_dl_tool.o"
$CC -o "$O/ipsw_dl" "$O/dl/ipsw_dl_tool.o" "$O/dl/ipsw_dl.o" "$O/curl_mbedtls.o" $O/obj/*.o ${CURLTOOL_LIBS--lpthread}
echo "linked  $O/ipsw_dl"
