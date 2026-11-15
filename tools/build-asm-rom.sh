#!/bin/bash
# Generic form of build-frame-probe-rom.sh's clang-based cross-assembly step, for any tests/roms/*.S synthetic
# firmware image (real AArch64 instructions, never Apple firmware -- see any such .S file's own header comment).
# Needs clang specifically: its integrated assembler takes -target aarch64-linux-gnu, so it can assemble AArch64
# syntax even when this machine is not arm64; plain cc/gcc (what a caller's inherited $CC often is here --
# tools/c1vm-test.sh defaults its own CC to "cc") does not understand -target at all. So this script always looks
# for clang itself and does NOT default to the inherited $CC; llvm-objcopy (or objcopy, if it understands aarch64
# here) then strips the ELF wrapper down to .text alone, which is exactly what the CPU is handed starting at
# VMAPPLE_FIRMWARE.
#
# Usage: [HV_CLANG=clang-18] tools/build-asm-rom.sh <in.S> <out.bin>
set -eu
cd "$(dirname "$0")/.."
IN=${1:?input .S path}; OUT=${2:?output .bin path}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

PICK_CC=""
for c in "${HV_CLANG:-}" clang clang-18 clang-17 clang-16 clang-15; do
  [ -n "$c" ] || continue
  if command -v "$c" >/dev/null 2>&1 && "$c" --version 2>/dev/null | grep -qi clang; then PICK_CC=$c; break; fi
done
[ -n "$PICK_CC" ] || { echo "build-asm-rom.sh: no clang found (need its integrated assembler for -target aarch64-linux-gnu; plain cc/gcc cannot assemble this)"; exit 1; }

"$PICK_CC" -target aarch64-linux-gnu -c -o "$TMP/rom.o" "$IN"

OBJCOPY=""
for c in llvm-objcopy llvm-objcopy-18 objcopy; do
  if command -v "$c" >/dev/null 2>&1 && "$c" -O binary --only-section=.text "$TMP/rom.o" "$TMP/try.bin" 2>/dev/null; then OBJCOPY=$c; break; fi
done
[ -n "$OBJCOPY" ] || { echo "build-asm-rom.sh: no objcopy here understands this object file (need llvm-objcopy or an aarch64-capable objcopy)"; exit 1; }
mv "$TMP/try.bin" "$OUT"
echo "build-asm-rom.sh: wrote $(wc -c < "$OUT") bytes to $OUT (assembled with $PICK_CC, stripped with $OBJCOPY) from $IN"
