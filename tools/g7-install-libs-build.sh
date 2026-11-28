#!/bin/bash
# The native programs the app's first-time setup buttons run (ROM extraction, IPSW download, HTTPS check), built the same way as the general APK workflow
# (.github/workflows/android-apk.yml) so that a single G7 APK can do the whole setup on a device with nothing installed.
#   libromextract.so  host/romextract.c   needs build-engine/romextract (tools/devices-build.sh already ran)       REQUIRED
#   libipswdl.so      tests/ipsw_dl_tool.c tools/ipswdl-build.sh, shipped configuration (no HV_DL_* test switch)    REQUIRED
#   libcurltool.so    the HTTPS/CA check program (tools/curltool-build.sh)                                         optional
# Usage: CC=<ndk clang> tools/g7-install-libs-build.sh <engine build dir> <jniLibs/arm64-v8a dir>
set -e
cd "$(dirname "$0")/.."
E=${1:?engine build dir (with romextract)}; J=${2:?jniLibs/arm64-v8a dir}; : "${CC:?CC}"
mkdir -p "$J"
[ -f "$E/romextract" ] || { echo "FAIL $E/romextract missing: tools/devices-build.sh did not build the ROM extractor"; exit 1; }
cp "$E/romextract" "$J/libromextract.so"
CURLTOOL_LIBS="" tools/ipswdl-build.sh build-ipswdl > /dev/null
cp build-ipswdl/ipsw_dl "$J/libipswdl.so"
if CURLTOOL_LIBS="" tools/curltool-build.sh build-curltool > /dev/null 2>&1 && [ -f build-curltool/curl_tool ]; then cp build-curltool/curl_tool "$J/libcurltool.so"; else echo "::warning::libcurltool.so did not build (optional HTTPS check button)"; fi
for f in libromextract.so libipswdl.so; do [ -s "$J/$f" ] || { echo "FAIL $J/$f is empty"; exit 1; }; echo "ok  $J/$f"; done
