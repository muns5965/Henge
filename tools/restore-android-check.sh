#!/bin/bash
# Survey, not a build: which of the macOS-restore libraries (third_party, as tools/build_ios_restore.sh builds them for iOS)
# compile for Android, one file at a time, and what an integrated link still lacks. Nothing here is run on a device.
# Stages, reported separately: (1) compile each file, (2) per-group result, (3) integrated link with the engine and a tiny main
# that references the installer entry points; unresolved symbols are listed by family. A curl_* family is expected (host/curl_shim.m is
# Objective-C and has no Android counterpart yet): it is an unresolved dependency, not an incompatibility of the libraries.
# Usage: CC=<ndk clang> tools/restore-android-check.sh <output dir>   (after engine-build.sh and devices-build.sh used the same dir)
set -u
OUT=${1:?output dir}
: "${CC:?CC}"
TP=third_party
R="$OUT/restore"
mkdir -p "$R"
COMMON="-O2 -w -DHAVE_CONFIG_H -I$TP/restore_config -I$TP -I$TP/libusb-1.0 -I$TP/curl_include -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libusbmuxd/include -I$TP/libimobiledevice/include -I$TP/libimobiledevice -I$TP/libtatsu/include -I$TP/libirecovery/include -I$TP/libzip/lib -I$TP/mbedtls/include"
declare -A OKN FAILN
OBJS=""
SUMMARY=""

group() { # name "extra flags" files...
  local name=$1 flags=$2; shift 2
  mkdir -p "$R/$name"
  local ok=0 bad=0 first=""
  for f in "$@"; do
    local o="$R/$name/$(basename "${f%.*}").o"
    if $CC $COMMON -I"$(dirname "$f")" $flags -c "$f" -o "$o" 2> "$o.log"; then
      ok=$((ok+1)); OBJS="$OBJS $o"
    else
      bad=$((bad+1)); echo "FAILED  $f"; head -5 "$o.log"
      [ -n "$first" ] || first=$(grep -m1 "error" "$o.log" | cut -c1-160)
    fi
  done
  SUMMARY="$SUMMARY
$(printf '%-12s compiled %3d  failed %3d  %s' "$name" "$ok" "$bad" "$first")"
}

# The same groups and flags as tools/build_ios_restore.sh.
group mbedtls "-I$TP/mbedtls/library" $TP/mbedtls/library/*.c
group plist "-I$TP/libplist/libcnary/include -DPACKAGE_VERSION=\"2.8.0-henge\"" $TP/libplist/src/*.c $TP/libplist/libcnary/*.c
group glue "-DPACKAGE_VERSION=\"1.3.3-henge\"" $TP/libimobiledevice-glue/src/*.c
group usbmuxdc "-DPACKAGE_VERSION=\"2.1.1-henge\" -DPACKAGE_STRING=\"libusbmuxd\"" $TP/libusbmuxd/src/*.c
group imd "-DHAVE_MBEDTLS -DPACKAGE_VERSION=\"1.4.0-henge\" -DPACKAGE_STRING=\"libimobiledevice\" -I$TP/libimobiledevice/src -I$TP/libimobiledevice/common" $TP/libimobiledevice/src/*.c $TP/libimobiledevice/common/*.c
group tatsu "-DPACKAGE_VERSION=\"1.0.5-henge\"" $TP/libtatsu/src/*.c
group irecv "-DPACKAGE_VERSION=\"1.3.1-henge\"" $TP/libirecovery/src/libirecovery.c
group muxd "-DPACKAGE_NAME=\"usbmuxd\" -DPACKAGE_VERSION=\"1.1.1-henge\" -Dmain=usbmuxd_main -Dlog_level=usbmuxd_log_level" $TP/usbmuxd/src/*.c
group idr "-I$TP/idevicerestore/src -DIDEVICERESTORE_NOMAIN -DPACKAGE_NAME=\"idevicerestore\" -DPACKAGE_VERSION=\"1.0.0-henge\"" $TP/idevicerestore/src/*.c
group henge "-std=gnu11 -D_GNU_SOURCE -Icore -Ihost -Idevices" host/libusb_shim.c host/install.c

# libzip through cmake with the NDK toolchain (the iOS script does the same with its own toolchain).
ZIPNOTE="not tried"
if command -v cmake >/dev/null && [ -n "${NDK_ROOT:-}" ]; then
  ZB="$R/libzip-build"
  if cmake -S $TP/libzip -B "$ZB" -DCMAKE_TOOLCHAIN_FILE="$NDK_ROOT/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 \
       -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DENABLE_COMMONCRYPTO=OFF -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF -DENABLE_OPENSSL=OFF \
       -DENABLE_WINDOWS_CRYPTO=OFF -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF \
       -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF -DLIBZIP_DO_INSTALL=OFF > "$ZB.cfg.log" 2>&1 \
     && cmake --build "$ZB" -j4 > "$ZB.build.log" 2>&1 && [ -f "$ZB/lib/libzip.a" ]; then
    ZIPNOTE="libzip.a built"; ZIPLIB="$ZB/lib/libzip.a"
  else
    ZIPNOTE="libzip FAILED (see logs)"; tail -5 "$ZB.cfg.log" "$ZB.build.log" 2>/dev/null
  fi
fi
SUMMARY="$SUMMARY
$(printf '%-12s %s' libzip "$ZIPNOTE")"

echo; echo "=== stage 1/2: compile, per group"; echo "$SUMMARY"

# Stage 3: integrated link. A tiny main that references the installer; the engine objects come from the earlier steps.
echo; echo "=== stage 3: integrated link (unresolved symbols by family)"
cat > "$R/linkmain.c" <<'EOC'
int hv_install_start(void *, const char *, const char *, unsigned long long, void *, void *, void *);
int usbmuxd_main(int, char **);
int main(void) { return hv_install_start(0, 0, 0, 0, 0, 0, 0) + usbmuxd_main(0, 0); }
EOC
ENG=""
for f in "$OUT"/dev_*.o "$OUT"/vm_*.o; do ENG="$ENG $f"; done
for o in classify operands translate dispatch mmu memop exception sysreg machine jit_region disk enter; do ENG="$ENG $OUT/$o.o"; done
$CC -O1 -w "$R/linkmain.c" $OBJS ${ZIPLIB:-} $ENG -lz -llog -Wl,--error-limit=0 -o "$R/linktest" > "$R/link.log" 2>&1
rc=$?
if [ $rc = 0 ]; then echo "linked"; else
  grep -o "undefined symbol: [A-Za-z0-9_]*" "$R/link.log" | sort -u | sed 's/undefined symbol: //' > "$R/undef.txt"
  echo "undefined symbols: $(wc -l < "$R/undef.txt")"
  echo "-- curl family (expected: no Android counterpart for host/curl_shim.m yet):"; grep -E "^curl_" "$R/undef.txt" | tr '\n' ' '; echo
  echo "-- everything else:"; grep -vE "^curl_" "$R/undef.txt" | tr '\n' ' '; echo
fi
echo "=== end"
exit 0
