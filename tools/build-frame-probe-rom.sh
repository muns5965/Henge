#!/bin/bash
# Assembles tests/roms/frame_probe.S (real AArch64 instructions, not Apple firmware -- see that file's header) into a raw
# firmware image ui_runner_vm can take as --rom, the same way the project's other stand-in ROM is a raw file of instruction
# words. This needs clang specifically (its integrated assembler takes -target aarch64-linux-gnu and so can assemble AArch64
# syntax even when this machine is not arm64; plain cc/gcc, which is what a caller's $CC often is here -- c1vm-test.sh
# defaults its own CC to "cc" -- does not understand -target at all). So this script always looks for clang itself and does
# NOT default to the inherited $CC; llvm-objcopy (or objcopy, if it understands aarch64 here) then strips the ELF wrapper
# down to .text alone, which is exactly what the CPU is handed starting at VMAPPLE_FIRMWARE.
#
# Usage: [HV_CLANG=clang-18] tools/build-frame-probe-rom.sh <out.bin>
set -eu
cd "$(dirname "$0")/.."
OUT=${1:?output .bin path}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

PICK_CC=""
for c in "${HV_CLANG:-}" clang clang-18 clang-17 clang-16 clang-15; do
  [ -n "$c" ] || continue
  if command -v "$c" >/dev/null 2>&1 && "$c" --version 2>/dev/null | grep -qi clang; then PICK_CC=$c; break; fi
done
[ -n "$PICK_CC" ] || { echo "build-frame-probe-rom.sh: no clang found (need its integrated assembler for -target aarch64-linux-gnu; plain cc/gcc cannot assemble this)"; exit 1; }

"$PICK_CC" -target aarch64-linux-gnu -c -o "$TMP/frame_probe.o" tests/roms/frame_probe.S

OBJCOPY=""
for c in llvm-objcopy llvm-objcopy-18 objcopy; do
  if command -v "$c" >/dev/null 2>&1 && "$c" -O binary --only-section=.text "$TMP/frame_probe.o" "$TMP/try.bin" 2>/dev/null; then OBJCOPY=$c; break; fi
done
[ -n "$OBJCOPY" ] || { echo "build-frame-probe-rom.sh: no objcopy here understands this object file (need llvm-objcopy or an aarch64-capable objcopy)"; exit 1; }
mv "$TMP/try.bin" "$OUT"

# sanity: the first bytes must be the first instruction this test relies on (mov x1, #0x70010000 -> encoding d2ae0021,
# little-endian bytes 21 00 ae d2); anything else means the source changed and the addresses baked into this script's
# caller (tools/c1vm-test.sh) no longer match.
head -c 4 "$OUT" | od -An -tx1 | tr -d ' \n' | grep -qx '2100aed2' || {
  echo "build-frame-probe-rom.sh: unexpected first instruction in $OUT (got $(head -c4 "$OUT" | od -An -tx1)); frame_probe.S and this script have drifted apart"
  exit 1
}
echo "build-frame-probe-rom.sh: wrote $(wc -c < "$OUT") bytes to $OUT (assembled with $PICK_CC, stripped with $OBJCOPY)"
