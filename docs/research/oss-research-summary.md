# OSS research summary (feature gaps) — 2026-11-27

Per-report details (sources, commit SHAs, file:line, per-file licence, READ vs INFER) are in the
individual reports in this directory. Henge is GPL, so GPL-derived facts may appear in the reports;
they are **host-side only** and must not enter the MIT library `libapplegfx-vulkan`.

| Report | Topic |
|---|---|
| oss-sampler-bits.md | constexpr sampler 64-bit word layout, sample operand order (+ addendum: bit comparison, dump statistics) |
| oss-air-sync-atomics.md | threadgroup memory, barriers, atomics, SIMD (32-wide vs Adreno 64), 0x42 |
| oss-render-tags.md | render/blit/object tags, 0x37 layout compatibility, compute 219, census corrections |
| oss-air-builtins.md | vertex/fragment built-ins, interpolation, depth/stencil/sample mask |
| oss-display-gamma.md | gamma descriptor, op 0x0a, device-info table, MMIO order, boot framebuffer/logo |
| oss-mmio.md, oss-bootfb.md, oss-display-events.md, oss-opcodes.md | earlier MMIO / boot FB / display event / opcode research |
| oss-adreno13.md, pipe13-analysis.md, oss-angle.md | the -13 pipeline-creation failure |
| license-audit.md | licence audit of library and sources |

## New meanings obtained
- Sampler word (external layout, unverified on device): bits 0-8 address s/t/r, 9-12 mag/min, 13-14 mip,
  15 pixel coordinates, 16-19 compare (8 = none), 20-23 aniso-1, 24-55 LOD clamps, 63 one-word marker.
  Bit 63 is NOT "normalized" (508dad5 misread; fixed in f0bd3cb).
- Sample operand order (verified against all 80 dumps): tex, samp, coord, [array], has_offset, offset, lod_is_level, lod/bias, min_lod, flags.
- 16 device render/blit tags match MIT upstream names/sizes; MIT segment-type numbering conflicts with the device.
- Gamma descriptor rides in the first ch4 op 0x08 only: offset 0x118000 (task-relative), length 0x4000, count 257; op 0x0a = 3x3 identity matrix.
- iBoot draws the boot logo; boot_args.Video values were never logged (diagnostic pending).

## Implemented from the research (library, branch research/g7-integ2 @ 42151bd, 120/120 tests)
X04 bias / explicit level / constant offset; constexpr sampler decode + Vulkan mapping; X11 integer intrinsics
(no occurrences in dumps: name-evidence only); GLSL.std.450 table audit; -13 pipeline matrix (LAGFX_PIPELINE_MATRIX=1).

## Still unknown
compare_func values other than 8, border colour, trailing sample flags, 0x42 record size (never observed), gamma table
element width/checksum, op 0x0c word 0x218, MMIO 0x1200/0x1220 meaning, which device-info tags are load-bearing,
Adreno behaviour for every -13 matrix row (needs a device run), min_lod / non-constant offsets (ignored with warning).

## Independence / licence limits
Sampler layout: from oosexgang/hakios-metal (MIT, but states Apple-binary provenance) and LuisaCompute (Apache-2.0);
only facts (bit positions) were used, no code. SPIRV-Cross MSL sampler enums do not match AIR bits. IOGraphics (APSL)
bullets in oss-display-gamma.md are used for public shapes only; drop them if not accepted.

## Census reclassification (pending)
D02 payload is {0,1}; op 0x0c word 4 is 0x4000; tag 14 (DrawIndexedPatches64) missing from census IDs; T13 "no meaning known"
is wrong for tags 28/29/138..152 (named in MIT). Full re-labelling (external-meaning-confirmed / own-verification /
implemented / device-verified) not yet done.

## Later reports in this directory (added 2026-11-27)
Five further reports now sit next to the ones above: oss-adreno-similar.md (public cases resembling the Adreno 840 -13 failure), oss-turnip-validation.md (whether Turnip, lavapipe and validation layers can help diagnose it), format-audit.md (Metal pixel/vertex format tables against the dumps and the public enums), red-compare.md (structural comparison of the failing and passing fragment modules) and green-frame-evidence.md (why the first frame was green). None of them contains a device result from a build newer than library commit 779802a, and none changes the census (docs/research/census.md carries the status updates up to ef4d456). What each establishes, and how far to trust it:
- oss-adreno-similar.md: eight public reports checked against primary pages; Cemu #2029 (fragment input without a matching vertex output, -13 on Adreno X1-85) is confirmed and is the closest match, the others are partly or wrongly described by the claims that prompted them (several are compute or SIGSEGV, not graphics -13); it ends with 15 candidate rows for the reduction ladder. Confidence: facts READ from the primary pages = medium-high; every statement that the cause applies to our module is inference = low.
- oss-turnip-validation.md: Turnip gen8 code exists upstream (Mesa >= 26.0) but working on a retail Adreno 840 via KGSL without root is unproven; lavapipe + validation on the failure bundles and spirv-val/spirv-reduce are feasible offline. Confidence: low-medium (Mesa source could not be read, Turnip-on-840 claims are third-party, many INFER items, nothing run on the device).
- format-audit.md: no Metal pixel format in the dumps is unmapped in the table that serves it, and it lists the unmapped vertex formats behind the P06 hardening. Confidence: high for what occurs in the dumps/logs, medium for the enum values (written from memory, not checked against an Apple header). Known flaw: its vertex-format counts come from the old byte-scan and miss Float4 (31) and Float (28), which real descriptors contain (tests/arm64-vertex-desc-real-unit.c).
- red-compare.md: the only structural trait shared by all failing fragment modules and absent from every passing one is the InputAttachment capability / OpImageRead; size, function and label counts only separate the sets by a gap of a few instructions. Confidence: low as an explanation (the failing modules are one shader in five id-numbering variants, so this is one observation; correlation, not cause).
- green-frame-evidence.md: the green (0,2,0) first frame matches bit for bit the Half4 colour attribute being read as float (0, 0.0078125, 0, 0). Confidence: high that the values match, medium that this is the cause (the shader was not traced, the captured image covers 546 of 1080 rows); the post-fix screen has NOT been verified.
