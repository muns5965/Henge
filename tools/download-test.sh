#!/bin/sh
# The real idevicerestore download.c on the HTTP stand-in (host/curl_mbedtls.c), against local servers. Usage: tools/download-test.sh [outdir]
set -e
cd "$(dirname "$0")/.."
O=${1:-build-dltest}
TP=third_party
CURLTOOL_CFLAGS=-DHV_ALLOW_CA_ENV tools/curltool-build.sh "$O" > /dev/null
CC=${CC:-cc}
$CC -std=gnu11 -D_GNU_SOURCE -O2 -w -DHAVE_CONFIG_H -I$TP/restore_config -I$TP/curl_include -I$TP/idevicerestore/src -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libimobiledevice/include -I$TP/libirecovery/include -I$TP \
  -DPACKAGE_NAME='"idevicerestore"' -DPACKAGE_VERSION='"1.0.0-henge"' \
  tests/test_download.c $TP/idevicerestore/src/download.c "$O/curl_mbedtls.o" $O/obj/*.o -lpthread -o "$O/test_download_legacy"
# the same file with the Android behaviour switched on (an Android build gets it from __ANDROID__)
$CC -std=gnu11 -D_GNU_SOURCE -O2 -w -DHV_SAFE_DOWNLOAD -DHAVE_CONFIG_H -I$TP/restore_config -I$TP/curl_include -I$TP/idevicerestore/src -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libimobiledevice/include -I$TP/libirecovery/include -I$TP \
  -DPACKAGE_NAME='"idevicerestore"' -DPACKAGE_VERSION='"1.0.0-henge"' \
  tests/test_download.c $TP/idevicerestore/src/download.c "$O/curl_mbedtls.o" $O/obj/*.o -lpthread -o "$O/test_download"
python3 tests/test_download.py "$O/test_download" "$O/dlwork" "$O/test_download_legacy"
