#!/bin/sh
# Builds the HTTP/TLS client test tool for the build host and runs tests/test_curl.py. Usage: tools/curl-test.sh [outdir]
set -e
cd "$(dirname "$0")/.."
O=${1:-build-curltest}
CURLTOOL_CFLAGS=-DHV_ALLOW_CA_ENV tools/curltool-build.sh "$O"
python3 tests/test_curl.py "$O/curl_tool" "$O/work"
