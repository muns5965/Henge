#!/usr/bin/env bash
# tests/gpu-validation/run_triangle_chain.sh
#
# A1-A5 chain for libapplegfx-vulkan's air2spv path on triangle.air, plus the
# stage-1 pixel harness and the negative controls.
#
# Required environment:
#   LIBAPPLEGFX_SRC    libapplegfx-vulkan source tree (read only; not modified)
#   LIBAPPLEGFX_LIB    directory containing libapplegfx-vulkan.a built from it
#   AIR_INPUT          path to triangle.air (examples/triangle/out/triangle.air)
#   WORK               EMPTY output directory (created if missing)
# Optional:
#   VK_ICD_FILENAMES   Vulkan ICD JSON (default: lavapipe if present)
#
# Exit 0 only if every step that ran passed. BLOCKED steps are reported and do
# not change the exit code (they are not claimed as PASS).
set -uo pipefail
: "${LIBAPPLEGFX_SRC:?set LIBAPPLEGFX_SRC}"
: "${LIBAPPLEGFX_LIB:?set LIBAPPLEGFX_LIB}"
: "${AIR_INPUT:?set AIR_INPUT}"
: "${WORK:?set WORK to an empty directory}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -e "$WORK" ] && [ -n "$(ls -A "$WORK" 2>/dev/null)" ]; then
  echo "WORK must be empty: $WORK" >&2; exit 2
fi
mkdir -p "$WORK"
if [ -z "${VK_ICD_FILENAMES:-}" ] && [ -f /usr/share/vulkan/icd.d/lvp_icd.json ]; then
  export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
fi
FAIL=0
BLOCKED=0
say() { echo "$*"; }

# --- build tools -----------------------------------------------------------
gcc -std=c11 -O1 -Wno-missing-field-initializers -o "$WORK/stage1_pixels" "$HERE/stage1_pixels.c" -lvulkan -lm \
  || { echo "BUILD stage1 FAILED"; exit 2; }
gcc -std=c11 -O1 -Wall -I"$LIBAPPLEGFX_SRC/src" -I"$LIBAPPLEGFX_SRC/include" -o "$WORK/air2spv_probe" \
  "$HERE/air2spv_probe.c" "$LIBAPPLEGFX_LIB/libapplegfx-vulkan.a" -lvulkan -lm || { echo "BUILD probe FAILED"; exit 2; }
gcc -std=c11 -O1 -I"$LIBAPPLEGFX_SRC/src" -I"$LIBAPPLEGFX_SRC/include" -o "$WORK/air2spv_stub_compare" \
  "$HERE/air2spv_stub_compare.c" "$LIBAPPLEGFX_LIB/libapplegfx-vulkan.a" -lvulkan -lm || { echo "BUILD stub_compare FAILED"; exit 2; }
gcc -std=c11 -O1 -Wno-missing-field-initializers -o "$WORK/draw_triangle" "$HERE/draw_triangle.c" -lvulkan -lm \
  || { echo "BUILD draw FAILED"; exit 2; }

# --- stage 1: independent pixels (no shader) -------------------------------
say "== stage1 pixel harness"
"$WORK/stage1_pixels" > "$WORK/stage1.txt" 2>&1; s1=$?
cat "$WORK/stage1.txt"; [ $s1 -eq 0 ] || FAIL=1

# --- A1/A2: AIR read and SPIR-V translation (fresh outputs) ---------------
say "== A1/A2 air2spv on triangle.air"
mkdir -p "$WORK/a2"
LAGFX_LOG_LEVEL=trace LAGFX_LOG_FILE="$WORK/a2/lagfx.log" \
  "$WORK/air2spv_probe" "$AIR_INPUT" "$WORK/a2" triangle_vertex:vertex triangle_fragment:fragment \
  > "$WORK/a2/probe.txt" 2>&1; p=$?
cat "$WORK/a2/probe.txt"
if grep -q "A1 module_open: status=0" "$WORK/a2/probe.txt"; then say "A1 RESULT: PASS"; else say "A1 RESULT: FAIL"; FAIL=1; fi
if [ $p -eq 0 ] && grep -q "triangle_vertex (vertex): status=0" "$WORK/a2/probe.txt" \
   && grep -q "triangle_fragment (fragment): status=0" "$WORK/a2/probe.txt"; then
  say "A2a translate_function (both stages): status=0"
else say "A2a RESULT: FAIL"; FAIL=1; fi
fb=$(grep -c "falling back\|emitting reference" "$WORK/a2/lagfx.log")
echo "A2b fallback markers in log: $fb"
"$WORK/air2spv_stub_compare" "$WORK/a2/triangle_vertex.spv" "$WORK/a2/triangle_fragment.spv" | tee "$WORK/a2/stub_compare.txt"
if [ "$fb" -eq 0 ] && ! grep -q "equals_.*=1" "$WORK/a2/stub_compare.txt"; then
  say "A2 RESULT: PASS (no fallback marker, no stub-equal blob)"
else say "A2 RESULT: FAIL (fallback marker or stub-equal blob)"; FAIL=1; fi
for f in triangle_vertex triangle_fragment; do
  magic=$(od -An -tx1 -N4 "$WORK/a2/$f.spv" | tr -d ' ')
  [ "$magic" = "03022307" ] && say "magic ok: $f" || { say "magic BAD: $f ($magic)"; FAIL=1; }
  sha256sum "$WORK/a2/$f.spv" | tee -a "$WORK/a2/hashes.txt"
done
sha256sum "$AIR_INPUT" | tee -a "$WORK/a2/hashes.txt"

# --- A3: SPIR-V validation --------------------------------------------------
say "== A3 spirv-val"
if command -v spirv-val >/dev/null 2>&1; then
  for f in triangle_vertex triangle_fragment; do
    spirv-val "$WORK/a2/$f.spv" && say "A3 $f: PASS" || { say "A3 $f: FAIL"; FAIL=1; }
  done
else
  BLOCKED=1
  say "A3 RESULT: BLOCKED (spirv-val not installed; A4/A5 results are not SPIR-V validity claims)"
fi

# --- A4/A5: pipeline and draw ----------------------------------------------
say "== A4/A5 triangle draw"
"$WORK/draw_triangle" "$WORK/a2/triangle_vertex.spv" "$WORK/a2/triangle_fragment.spv" > "$WORK/a45.txt" 2>&1; d=$?
cat "$WORK/a45.txt"; [ $d -eq 0 ] || FAIL=1

# --- negative control: draw call removed must FAIL -------------------------
say "== negative control (draw removed; expected FAIL)"
sed 's/    vkCmdDraw(cb, 3, 1, 0, 0);/    \/* NEG: draw removed *\//' "$HERE/draw_triangle.c" > "$WORK/neg_nodraw.c"
gcc -std=c11 -O1 -Wno-missing-field-initializers -o "$WORK/neg_nodraw" "$WORK/neg_nodraw.c" -lvulkan -lm
"$WORK/neg_nodraw" "$WORK/a2/triangle_vertex.spv" "$WORK/a2/triangle_fragment.spv" > "$WORK/neg_nodraw.txt" 2>&1; n=$?
tail -1 "$WORK/neg_nodraw.txt"
[ $n -ne 0 ] && say "negative control detected the missing draw (exit $n)" || { say "negative control NOT detected"; FAIL=1; }

if [ $FAIL -ne 0 ]; then echo "OVERALL: FAIL"; elif [ $BLOCKED -ne 0 ]; then echo "OVERALL: PASS for executed steps; A3 BLOCKED"; else echo "OVERALL: PASS"; fi
exit $FAIL
