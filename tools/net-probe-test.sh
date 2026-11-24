#!/bin/bash
# hv_curl_probe (host/curl_mbedtls.c) and the probe program (host/net_probe.c) against local servers only; nothing leaves this machine, nothing is sent to Apple.
# Usage: [CC=cc] tools/net-probe-test.sh [build dir]
set -eu
cd "$(dirname "$0")/.."
O=${1:-build-netprobe}; CC=${CC:-cc}; TP=third_party
CURLTOOL_CFLAGS=-DHV_ALLOW_CA_ENV tools/curltool-build.sh "$O" > /dev/null
$CC -std=gnu11 -O1 -D_GNU_SOURCE -Ihost -c tests/test_net_probe.c -o "$O/test_net_probe.o"
$CC -o "$O/test_net_probe" "$O/test_net_probe.o" "$O/curl_mbedtls.o" $O/obj/*.o -lpthread -lm
python3 tests/test_net_probe.py "$O/test_net_probe" "$O/np-work"
# the probe program itself must build the way the shipped binary is built (no test switches)
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -D_GNU_SOURCE -Ihost -c host/net_probe.c -o "$O/net_probe.o"
$CC -std=gnu11 -O2 -w -D_GNU_SOURCE -Ihost -I$TP/curl_include -I$TP/mbedtls/include -c host/curl_mbedtls.c -o "$O/curl_ship.o"
$CC -o "$O/net_probe" "$O/net_probe.o" "$O/curl_ship.o" $O/obj/*.o -lpthread -lm
echo "linked  $O/net_probe (not run here: it would contact the Apple addresses)"
