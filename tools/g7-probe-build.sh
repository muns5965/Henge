#!/bin/bash
# Builds the g7_probe preflight program (host/g7_probe.c) for the APK (Android arm64 via CC=aarch64-linux-android30-clang) or for the host, then runs its static check.
# It links no engine, restore, network or disk-creation code (SHIPPED constants only: no test defines). Usage: CC=<compiler> [NM=<nm>] [READELF=<readelf>] tools/g7-probe-build.sh <output dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?output dir}; CC=${CC:?CC}; mkdir -p "$OUT"
CF="-std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections -Icore -Ihost -Idevices"
$CC $CF host/g7_probe.c host/runner_g7_core.c host/g7_preflight.c host/disk_policy.c host/crypto_portable.c -Wl,--gc-sections -o "$OUT/g7_probe" 2> "$OUT/g7_probe.log" \
  || { echo "FAILED  g7_probe"; head -30 "$OUT/g7_probe.log"; exit 1; }
echo "linked  $OUT/g7_probe"
tools/check-g7-probe-safe.sh "$OUT/g7_probe"
