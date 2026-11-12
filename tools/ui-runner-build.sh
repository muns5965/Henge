#!/bin/bash
# C1-1: builds the runner (host/ui_runner.c, synthetic mode: no virtual machine, no restore stack, no network code) and checks the binary (tools/check-runner-safe.sh).
# Usage: CC=<compiler> [NM=<nm>] [PF_THREADLIB="" on Android: bionic has no libpthread] tools/ui-runner-build.sh <output dir>
set -u
cd "$(dirname "$0")/.."
OUT=${1:?output dir}; CC=${CC:-cc}
mkdir -p "$OUT"
if $CC -std=gnu11 -O2 -Wall -Wextra -Werror host/ui_runner.c host/frame_shm.c host/display_buf.c -lm ${PF_THREADLIB--lpthread} -o "$OUT/ui_runner" 2> "$OUT/ui_runner.log"; then
  echo "linked  $OUT/ui_runner"
else
  echo "FAILED  link ui_runner"; head -30 "$OUT/ui_runner.log"; exit 1
fi
tools/check-runner-safe.sh "$OUT/ui_runner"
