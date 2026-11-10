#!/bin/bash
# vmrun with the TSS probe in it (--tss-probe): vmrun-irecv's pieces + libplist + libtatsu (HV_TSS_HTTPS_ONLY: https only, one attempt on request) + the HTTP client
# stand-in with mbedtls + libzip + host/remote_zip.c + host/tss_probe_core.c + host/tss_probe.c. A separate program on purpose: if it does not build, everything else ships.
# NO test switches (no HV_TSS_ALLOW_HTTP_TEST, HV_RZ_ALLOW_HTTP_TEST, HV_ALLOW_CA_ENV): tools/check-no-test-flags.sh scans this file.
# Run after tools/engine-build.sh and tools/devices-build.sh with the same compiler and output dir. For Android set NDK_ROOT (libzip is built with the NDK toolchain file).
# Usage: CC=<compiler> [NDK_ROOT=..] [EXTRA_LIBS=...] tools/vmrun-tss-build.sh <output dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?output dir}
: "${CC:?CC}"
TP=third_party
R="$OUT/tssprobe"
mkdir -p "$R"
CURLTOOL_LIBS="" tools/curltool-build.sh "$R/mb" > "$R/mb.log" 2>&1 || { echo "FAILED  mbedtls/curl stand-in"; tail -5 "$R/mb.log"; exit 1; }
ZB="$R/libzip-build"
if [ ! -f "$ZB/lib/libzip.a" ]; then
  TC=""; [ -n "${NDK_ROOT:-}" ] && TC="-DCMAKE_TOOLCHAIN_FILE=$NDK_ROOT/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30"
  cmake -S $TP/libzip -B "$ZB" $TC -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DENABLE_COMMONCRYPTO=OFF -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF -DENABLE_OPENSSL=OFF \
    -DENABLE_WINDOWS_CRYPTO=OFF -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF \
    -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF -DLIBZIP_DO_INSTALL=OFF > "$R/libzip.cfg.log" 2>&1 \
    && cmake --build "$ZB" -j4 > "$R/libzip.build.log" 2>&1 || { echo "FAILED  libzip"; tail -5 "$R/libzip.cfg.log" "$R/libzip.build.log"; exit 1; }
fi
FL="-std=gnu11 -D_GNU_SOURCE -O2 -g -w -DHAVE_CONFIG_H -Icore -Ihost -Idevices -I$TP/restore_config -I$TP -I$TP/libusb-1.0 -I$TP/libirecovery/include -I$TP/libimobiledevice-glue/include -I$TP/libplist/include -I$TP/libplist/libcnary/include -I$TP/libtatsu/include -I$TP/curl_include -I$ZB -I$TP/libzip/lib"
objs=""
cc1() { # src extra-flags
  local f=$1; shift
  local o="$R/$(basename "$(dirname "$f")")_$(basename "${f%.*}").o"
  $CC $FL "$@" -c "$f" -o "$o" 2> "$o.log" || { echo "FAILED  $f"; head -20 "$o.log"; exit 1; }
  objs="$objs $o"
}
for f in $TP/libimobiledevice-glue/src/collection.c $TP/libimobiledevice-glue/src/thread.c; do cc1 $f; done
cc1 $TP/libirecovery/src/libirecovery.c '-DPACKAGE_VERSION="1.3.1-henge"'
for f in host/libusb_shim.c host/remote_zip.c; do cc1 $f; done
cc1 host/tss_probe_core.c -DHV_TSS_HTTPS_ONLY
cc1 host/tss_probe.c
for f in $TP/libplist/src/*.c $TP/libplist/libcnary/node.c $TP/libplist/libcnary/node_list.c; do cc1 $f -I"$(dirname "$f")" '-DPACKAGE_VERSION="2.8.0-henge"'; done
for f in $TP/libtatsu/src/*.c; do cc1 $f -I$TP/libtatsu/src '-DPACKAGE_VERSION="1.0.5-henge"' -DHV_TSS_HTTPS_ONLY; done
$CC $FL -DHV_TSS_PROBE -c host/vmrun.c -o "$R/vmrun.o" 2> "$R/vmrun.o.log" || { echo "FAILED  host/vmrun.c"; head -20 "$R/vmrun.o.log"; exit 1; }
eobjs=""; for o in classify operands translate dispatch mmu memop exception sysreg machine jit_region disk enter; do eobjs="$eobjs $OUT/$o.o"; done
dobjs="$(ls $OUT/dev_*.o)"
vobjs="$OUT/vm_vm.o $OUT/vm_stubs_portable.o $OUT/vm_usb_host.o"
if $CC $FL "$R/vmrun.o" $objs "$R/mb/curl_mbedtls.o" $R/mb/obj/*.o "$ZB/lib/libzip.a" $vobjs $eobjs $dobjs -lz -lm ${EXTRA_LDFLAGS:-} ${EXTRA_LIBS:--lpthread} -o "$OUT/vmrun_tss" 2> "$R/link.log"; then
  echo "linked  $OUT/vmrun_tss"
else
  echo "FAILED  link vmrun_tss"; head -30 "$R/link.log"; exit 1
fi
