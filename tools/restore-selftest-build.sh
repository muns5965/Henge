#!/bin/bash
# Builds the whole restore stack plus tests/test_restore_stack.c into OUT/restore_selftest (vendored sources, as tools/build_ios_restore.sh does for iOS).
# Native (Linux, any arch) by default; for Android set CC to the NDK clang and NDK_ROOT (libzip is then built with the NDK toolchain file).
# Usage: [CC=cc] [NDK_ROOT=..] [EXTRA_LIBS=..] tools/restore-selftest-build.sh OUT
set -u
cd "$(dirname "$0")/.."
OUT=${1:?output dir}
CC=${CC:-cc}
TP=third_party
R="$OUT/rs"
mkdir -p "$R/obj"
COMMON="-O1 -w -DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/libusb-1.0 -I$TP/curl_include -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libusbmuxd/include -I$TP/libimobiledevice/include -I$TP/libimobiledevice -I$TP/libtatsu/include -I$TP/libirecovery/include -I$TP/mbedtls/include -I$TP/libzip/lib -Ihost -Idevices -Icore"
FAIL=0
add() { # name "flags" files...   (compiles each file into $R/obj/<name>_<base>.o; up to $(nproc) at a time)
  local name=$1 flags=$2; shift 2
  for f in "$@"; do
    local o="$R/obj/${name}_$(basename "${f%.*}").o"
    while [ "$(jobs -rp | wc -l)" -ge "$(nproc)" ]; do wait -n || FAIL=1; done
    ( eval "$CC $COMMON -I$(dirname "$f") $flags -c $f -o $o" 2> "$o.log" || { echo "FAILED  $f"; head -5 "$o.log"; exit 1; } ) &
  done
}
add mbedtls "-I$TP/mbedtls/library" $TP/mbedtls/library/*.c
add plist "-I$TP/libplist/libcnary/include -DPACKAGE_VERSION='\"2.8.0-henge\"'" $TP/libplist/src/*.c $TP/libplist/libcnary/*.c
add glue "-DPACKAGE_VERSION='\"1.3.3-henge\"'" $TP/libimobiledevice-glue/src/*.c
add usbmuxdc "-DPACKAGE_VERSION='\"2.1.1-henge\"' -DPACKAGE_STRING='\"libusbmuxd\"'" $TP/libusbmuxd/src/*.c
add imd "-DHAVE_MBEDTLS -DPACKAGE_VERSION='\"1.4.0-henge\"' -DPACKAGE_STRING='\"libimobiledevice\"' -I$TP/libimobiledevice/src -I$TP/libimobiledevice/common" $TP/libimobiledevice/src/*.c $TP/libimobiledevice/common/*.c
add tatsu "-DHV_TSS_HTTPS_ONLY -DPACKAGE_VERSION='\"1.0.5-henge\"'" $TP/libtatsu/src/*.c
add irecv "-DPACKAGE_VERSION='\"1.3.1-henge\"'" $TP/libirecovery/src/libirecovery.c
add muxd "-DPACKAGE_NAME='\"usbmuxd\"' -DPACKAGE_VERSION='\"1.1.1-henge\"' -Dmain=usbmuxd_main -Dlog_level=usbmuxd_log_level" $TP/usbmuxd/src/*.c

# libzip (idevicerestore reads the IPSW with it); its generated zipconf.h is needed by idevicerestore's sources
ZB="$R/libzip-build"
TCF=""; [ -n "${NDK_ROOT:-}" ] && TCF="-DCMAKE_TOOLCHAIN_FILE=$NDK_ROOT/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30"
cmake -S $TP/libzip -B "$ZB" $TCF -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DENABLE_COMMONCRYPTO=OFF -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF -DENABLE_OPENSSL=OFF \
  -DENABLE_WINDOWS_CRYPTO=OFF -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF \
  -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF -DLIBZIP_DO_INSTALL=OFF > "$R/libzip.cfg.log" 2>&1 && cmake --build "$ZB" -j4 > "$R/libzip.build.log" 2>&1 \
  && [ -f "$ZB/lib/libzip.a" ] || { echo "FAILED libzip"; tail -5 "$R/libzip.cfg.log" "$R/libzip.build.log"; exit 1; }
add idr "-I$ZB -I$TP/idevicerestore/src -DIDEVICERESTORE_NOMAIN -DHV_SAFE_DOWNLOAD -DHV_STOP_BEFORE_ROOTFS -DPACKAGE_NAME='\"idevicerestore\"' -DPACKAGE_VERSION='\"1.0.0-henge\"'" $TP/idevicerestore/src/*.c
add henge "-std=gnu11 -D_GNU_SOURCE" host/libusb_shim.c host/install.c host/usb_host.c host/curl_mbedtls.c devices/virtio_usb.c
add test "-std=gnu11 -D_GNU_SOURCE" tests/test_restore_stack.c

while [ "$(jobs -rp | wc -l)" -gt 0 ]; do wait -n || FAIL=1; done
[ $FAIL = 0 ] || { echo "FAILED a compile step"; exit 1; }
# archive like the iOS build, so libcnary's own main is only linked if something asks for it
AR=${AR:-ar}; [ -n "${NDK_ROOT:-}" ] && AR="$(dirname "$CC")/llvm-ar"
rm -f "$R/lib.a"
"$AR" rcs "$R/lib.a" $(ls $R/obj/*.o | grep -v "/test_")
if $CC -O1 -w "$R/obj/test_test_restore_stack.o" "$R/lib.a" "$ZB/lib/libzip.a" -lz -lm ${EXTRA_LIBS--lpthread} -o "$OUT/restore_selftest" > "$R/link.log" 2>&1; then
  echo "linked  $OUT/restore_selftest"
else
  echo "FAILED  link restore_selftest"; grep -o "undefined [a-z ]*[:] [A-Za-z0-9_.]*\|undefined reference to .*" "$R/link.log" | sort -u | head -30; head -20 "$R/link.log" | cut -c1-200; exit 1
fi
