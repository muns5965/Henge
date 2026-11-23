#!/bin/bash
# The G7 runner (host/g7_runner.c) in its two builds. Usage: CC=<compiler> [NM=..] [READELF=..] [EXTRA_LIBS=..] [PF_THREADLIB=""] tools/g7-runner-build.sh default|real <output dir> [<engine dir> <restore g7 dir>]
#   default: no execution stage; links the preflight and the install-disk policy only; the static check of g7_probe applies (tools/check-g7-probe-safe.sh).
#   real:    -DHV_G7_REAL_EXECUTION: host/g7_runner_real.c (the real disk creator, the machine, the restore tool WITHOUT any stop switch: tools/restore-g7-build.sh). Needs the engine and
#            device objects (tools/engine-build.sh, tools/devices-build.sh) and the G7 restore stack. For the link check on ARM64 only: no APK step calls this with `real`, and running the
#            result makes Apple requests and writes the disk (a separately approved step).
set -u
cd "$(dirname "$0")/.."
MODE=${1:?default|real}; OUT=${2:?output dir}; CC=${CC:?CC}; mkdir -p "$OUT"
CF="-std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -Icore -Ihost -Idevices"
case $MODE in
default)
  $CC $CF host/g7_runner.c host/runner_g7_core.c host/g7_preflight.c host/disk_policy.c host/crypto_portable.c -Wl,--gc-sections -o "$OUT/g7_runner" 2> "$OUT/g7_runner.log" \
    || { echo "FAILED  g7_runner (default)"; head -30 "$OUT/g7_runner.log"; exit 1; }
  echo "linked  $OUT/g7_runner (default: no execution stage)"
  tools/check-g7-probe-safe.sh "$OUT/g7_runner"
  ;;
real)
  E=${3:?engine build dir}; RS=${4:?restore g7 build dir (tools/restore-g7-build.sh)}
  TP=third_party
  LIB="$RS/rs/lib.a"; ZIP="$RS/rs/libzip-build/lib/libzip.a"
  [ -f "$LIB" ] && [ -f "$ZIP" ] || { echo "FAILED  the G7 restore stack is not built in $RS (tools/restore-g7-build.sh)"; exit 1; }
  eobjs=""; for o in classify operands translate dispatch mmu memop exception sysreg machine jit_region disk enter; do eobjs="$eobjs $E/$o.o"; done
  dobjs="$(ls $E/dev_*.o)"
  vobjs="$E/vm_vm.o $E/vm_stubs_portable.o $E/vm_usb_host.o"
  for o in $eobjs $vobjs; do [ -f "$o" ] || { echo "FAILED  missing $o (run the engine and devices builds first)"; exit 1; }; done
  FL="-std=gnu11 -D_GNU_SOURCE -O2 -g -w -DHAVE_CONFIG_H -DHV_G7_REAL_EXECUTION -Icore -Ihost -Idevices -I$TP/restore_config -I$TP -I$TP/mbedtls/include -I$TP/libtatsu/include -I$TP/libusb-1.0 -I$TP/curl_include -I$TP/libplist/include -I$TP/libimobiledevice-glue/include -I$TP/libirecovery/include -I$TP/idevicerestore/src"
  R="$OUT/g7real"; mkdir -p "$R"; objs=""
  for f in g7_runner g7_runner_real g7_ipsw_check runner_g7_core g7_preflight g7_restore; do   # install.c and disk_policy.c come from the restore stack archive (built with no G switch); hv_sha256 and hv_aes_crypt from the device objects
    $CC $FL -c host/$f.c -o "$R/$f.o" 2> "$R/$f.log" || { echo "FAILED  host/$f.c"; head -20 "$R/$f.log"; exit 1; }
    objs="$objs $R/$f.o"
  done
  if $CC $FL $objs $vobjs $eobjs $dobjs "$LIB" "$ZIP" ${EXTRA_LDFLAGS:-} ${EXTRA_LIBS:-} -lz -lm ${PF_THREADLIB--lpthread} -o "$OUT/g7_runner_real" 2> "$R/link.log"; then
    echo "linked  $OUT/g7_runner_real (execution build: NOT packaged anywhere)"
  else
    echo "FAILED  link g7_runner_real"; grep -o "undefined reference to .*" "$R/link.log" | sort -u | head -30; head -30 "$R/link.log" | cut -c1-200; exit 1
  fi
  ;;
*) echo "usage: g7-runner-build.sh default|real <out> ..."; exit 64;;
esac
