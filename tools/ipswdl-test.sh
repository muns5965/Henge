#!/bin/sh
# Builds host/ipsw_dl.c twice - a test build (loopback http, test arguments) and the shipped configuration (no HV_DL_* switch) - and runs tests/test_ipsw_dl.py
# against local mock Range servers. Nothing leaves the machine. Usage: [CC=..] tools/ipswdl-test.sh [outdir]
set -e
cd "$(dirname "$0")/.."
O=${1:-build-ipswdl}; CC=${CC:-cc}; TP=third_party
DL_LIB_FLAGS=-DHV_DL_ALLOW_HTTP_TEST DL_TOOL_FLAGS=-DHV_DL_TEST CC=$CC tools/ipswdl-build.sh "$O" > /dev/null
mkdir -p "$O/ship"
$CC -O2 -Wall -std=gnu11 -D_GNU_SOURCE -Ihost -I$TP/curl_include -I$TP/mbedtls/include -c host/ipsw_dl.c -o "$O/ship/ipsw_dl.o"
$CC -O2 -Wall -std=gnu11 -Ihost -c tests/ipsw_dl_tool.c -o "$O/ship/ipsw_dl_tool.o"
$CC -o "$O/ipsw_dl_ship" "$O/ship/ipsw_dl_tool.o" "$O/ship/ipsw_dl.o" "$O/curl_mbedtls.o" $O/obj/*.o -lpthread
echo "linked  $O/ipsw_dl (test) and $O/ipsw_dl_ship"
python3 tests/test_ipsw_dl.py "$O/ipsw_dl" "$O/ipsw_dl_ship" "$O/work"
