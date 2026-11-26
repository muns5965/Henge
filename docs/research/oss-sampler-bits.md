# OSS evidence: AIR constexpr-sampler word + air.sample_texture_* operands
Researched 2026-11-27, read-only. Clones were shallow/blobless, now deleted.

## Verdict
Two open, per-file-MIT/Apache projects document BOTH. No spec, SPIRV-Cross, MoltenVK or Mesa(upstream) document them (see "Negative").
Caveat on provenance of source A (below): the authors say they got the layout by disassembling Apple binaries.

## Sources used
| id | repo@commit | licence | status |
|----|-------------|---------|--------|
| A | oosexgang/hakios-metal @ d36f329e2684a1968b7c1a0ceee1661472cb707a (path window-server/tools/mesa/air/, tools/kernels/) | MIT (LICENSE; SPDX MIT in each file header read; NOTICE says kernels are hand-written) | READ |
| B | LuisaGroup/LuisaCompute @ aa169200d19d29f691a6f34da916ff61d2931d59 (src/backends/metal4/llvm_codegen/) | Apache-2.0 (root LICENSE; no per-file headers in the files read) | READ |
| C | vertex-language/air @ 3d277d16858a7141eb021fd614f021b595c13978 | MIT | READ |
| D | KhronosGroup/SPIRV-Cross @ f0edde12d69ea7a498da14af05ec14f390b07c98 | Apache-2.0 | READ |

Skipped / not read: nullmoth/nvidia-macos-driver (PolyForm Noncommercial: not allowed; only seen as a search-hit snippet, nothing used); oosexgang/hakios-xnu-kernel (APSL/GPL mix, per-component licence unclear: skipped); PureDarwin/PureDarwin mslc/airify.cpp (licence unclear: skipped); steelbrain/metal2vulkan (forbidden); superboyyy/hakios-arm64e-dada, soumarcelino/fiio-jm21-xnu, jesvi-eulah/Jesvi-iOS, danstronics/ios-s9plus-exynos (copies of the xnu tree, licence unclear: skipped); DsoTsin/gamedev (no licence, irrelevant).
Provenance warning for A: nir_air.h:58-79 states the field positions were taken from Apple's GPUCompiler.framework metal_stdlib header, libGPUCompilerImpl (AIRSamplerStateEncoder::encode) and AGXCompilerCore (AIRConstSampler::constructFlatReply), i.e. from Apple binaries. The file is MIT-licensed, but if your policy forbids facts derived from disassembly, treat A as "licence ok, provenance not clean" and use B (observed Apple-compiler output regenerated via MSL oracle) for corroboration. Repo docs also mention metal2vulkan as a sibling path (unrelated to the files read).

## 1. constexpr sampler 64-bit word (X06)
### A: air_to_nir.c:5742-5766 (air_decode_sampler_state) + nir_air.h:58-114. READ. Confidence high.
Paraphrase of the layout (w = the i64 in `@__air_sampler_state`, named in `!air.sampler_states = !{!{"air.sampler_state", ptr @__air_sampler_state}}`):
- bits 0-2 / 3-5 / 6-8: address mode for s / t / r. Values: 0 clamp_to_zero (transparent black, NOT border), 1 clamp_to_edge (default), 2 repeat, 3 mirrored_repeat, 4 clamp_to_border.
- bits 9-10: mag filter (0 nearest, 1 linear, 2 bicubic); bits 11-12: min filter (same values).
- bits 13-14: mip filter (0 none, 1 nearest, 2 linear).
- bit 15: coord::pixel (unnormalized) when set; 0 = normalized (the default).
- bits 16-19: compare_func (4 bits, enum NOT given; default is 8, see below).
- bits 20-23: max_anisotropy minus 1 (so 1..16).
- bits 24-39: lod_min as raw half bits; bits 40-55: lod_max as raw half bits (default 0x0000 / 0x7bff = [0, 65504]).
- bits 56-57: border_color (enum not given); bits 58-59: "reduction" (AGX does not read it).
- bit 63: marks the one-word form (nir_air.h / comment at air_to_nir.c:5744). The two-word form (second word's low 16 bits = LOD bias per A) is "reported rather than decoded".
Self-check claimed by A (nir_air.h:70-79): the three words in its 456-module corpus decode to sampler(coord::pixel), sampler(coord::pixel, filter::linear), sampler(coord::pixel, filter::linear, address::clamp_to_zero).
Also A tools/kernels/tri_ctex_fs.ll (header comment): `constexpr sampler s(address::repeat, filter::nearest)` = word 34901797600493650 = 0x007bff0000000052 (s,t repeat, r edge: 2 | 2<<3 | 1<<6 = 0x52; pre-bit63, hand-written, MIT, "nothing from Apple but the shape").

### B: AIR_INTRINSICS_AND_TRANSLATION.md:1338-1392 (section 8.7) and metal_codegen_llvm_atomic.cpp:478-512 (descriptor table). READ. Confidence high (independent, values "regenerated with Apple Metal 32023.883 via an MSL 4.0 differential oracle").
Observed words (low 56 bits; bit 63 is ORed in only for AIR 2.5/2.6 scalar i64; AIR >= 2.7 uses `[2 x i64] [desc, 0]` with no bit 63; address space 2, align 8, internal constant, one `!air.sampler_states` node per state):
- 0x007bff0000080049 / ...080092 / ...0800db / ...080000 = point filter, mip none, address edge / repeat / mirror / zero. => confirms A's address fields (0x49=1,1,1; 0x92=2,2,2; 0xdb=3,3,3; 0=zero) and your hypothesis for bits 0..8.
- +0x2a00 (bits 9,11,13) = linear min+mag, mip nearest; +0x4a00 (bits 9,11,14) = linear min+mag, mip linear; +0x0a00 (bits 9,11) = linear, mip none (the swapchain sampler 0x007bff0000080a49, builtin.cpp:841-846). => confirms A's mag/min/mip field split.
- 0x007bff0000f84a49 = anisotropic: differs from linear-mip edge by +0xf00000 = bits 20-23 = 15 (=> max_anisotropy 16, consistent with A "+1").
- bit 19 (0x80000) is set in EVERY Apple-emitted word => compare_func nibble = 8 = default "no comparison" value (INFER from A's field position + B constants).
Decode of your four words with A's layout (INFER, mechanically checked):
- 0x807bff0000088a00: bit63; addr 0,0,0 = clamp_to_zero; linear/linear; mip none; bit15 = PIXEL coords; cmp 8; aniso 1; lod [0, 0x7bff].
- 0x807bff0000088000: same but nearest (mag=min=0): pixel, nearest, clamp_to_zero.
- 0x807bff0000080092: normalized, nearest, mip none, repeat s/t/r.
- 0x807bff0000080a49: normalized, linear, mip none, clamp_to_edge.
CORRECTION to your working notes: per A, bit 63 is the one-word-form marker, NOT "normalized coords". Normalized vs pixel is bit 15 (set = pixel). Your words with bit 15 set are unnormalized (pixel) samplers (plausible for blur kernels). Your note "bit 15 maybe linear filter" is wrong per A+B: linear is bits 9-10 and 11-12 (0x0a00 pattern); bit 15 is coord::pixel. Your note "bits 0..8 three 3-bit fields" is confirmed. Note Vulkan mapping: Vulkan unnormalizedCoordinates needs clamp_to_edge/border, no mips, no anisotropy; clamp_to_zero + pixel therefore needs a shader-side/border-colour workaround (Metal semantics, INFER).

### Still undocumented in any allowed source
- compare_func enum (bits 16-19) other than default 8; border_color enum (bits 56-57: only mentioned that clamp_to_border = address 4, colours not enumerated); reduction (58-59); bicubic (mag/min = 2) semantics; whether bit 9 vs 11 is mag vs min is from A only (B always sets both together); two-word form's LOD-bias use (A says AIR 2.6, B says [2 x i64] starts at AIR 2.7: version boundary disagrees).

## 2. air.sample_texture_{1d,2d,3d,cube,*_array} operands (X04)
### A: air_intrinsics.c:1770-1830 (header comment), 2370-2535 (reader). READ. Confidence high; signatures "measured from the corpus" and checked against Apple's metal_texture header per the comment.
Plain form, operand order (brackets optional per dimension):
`sample_<kind>_<dim>(tex, samp, [plane,] coord, [i32 array_index,] [i1 has_offset, offset,] i1 lod_is_level, float lod_or_bias, float min_lod, i32 flags) -> { <4 x T>, i8 }`
- compare (sample_compare_depth_*): the compare value (float) follows coord (and array index), before the offset pair, in both plain and _grad forms. Return is { float, i8 } (one value). air.sample_depth_2d.f32 additionally carries an i32 "aspect selector" between sampler and coordinate (comment at ~2392-2403).
- offset: only present for 1d/2d/3d (+arrays), NOT cube/cube_array. Offset type is <N x i32> (scalar i32 for 1d). The i1 before it is "has_offset"; reader only applies the offset if the flag is true AND the value is not all zero. Offset is an immediate-style constant in Metal.
- level/bias: i1 lod_is_level: true -> the float is an explicit level (Metal level(x)), false -> the float is a bias (bias(x)); (false, 0.0) = plain implicit-LOD sample. Corpus mention: Resident Evil 4 has 23,823 level-form and 5,961 bias-form calls.
- min_lod: the second float (Metal min_lod_clamp), 0.0 = none.
- gradient form: name suffix _grad on the dimension (air.sample_texture_2d_grad.v4f32 / _2d_array_grad): `(tex, samp, [plane,] coord, [array,] dPdx, dPdy, float min_lod, [i1 has_offset, offset,] i32 flags)`. No lod/bias; gradients have the coordinate's components WITHOUT the array layer. NOTE the reorder vs the plain form.
- trailing i32 flags: always 0 in every example; A does not interpret it (UNKNOWN meaning).
- 1d: `air.sample_texture_1d.v4f32(tex, samp, float, i1, i32 offset, i1, float, float, i32)` (tools/kernels/tri_tex1d_fs.ll); cube: `(tex, samp, <3 x float>, i1 lod_flag, float, float, i32)` (tri_cube_fs.ll); 2d_array: `(tex, samp, <2 x float>, i32 layer, i1, <2 x i32>, i1, float, float, i32)` (tri_tex2da_fs.ll).
- vertex stage: hand-kernel tri_vtex_vs.ll passes lod_is_level=true, lod 0.0 (Metal requires explicit level in the vertex stage). That matches your X04 note "explicit Lod 0 in the vertex stage".
- status field (second struct element, i8): A says it is Metal's sparse-residency byte; no corpus module reads it; A produces zero. Same in B ("the status byte is ignored"). So translating it as constant 0 (instead of OpUndef) is the documented-safe choice. READ for "sparse residency" (A comment at air_intrinsics.c:1826-1829); producing 0 is A's practice.

### B: AIR_INTRINSICS_AND_TRANSLATION.md:1314-1333, metal_codegen_llvm_resource.cpp:369-410 and 479-515. READ. Confidence high.
- `air.sample_texture_Dd.v4f32(tex, smp, coord, i1 true, <D x i32> zero_offset, i1 has_explicit_lod, float lod, float 0.0, i32 0)` -- same order as A (B does not name the first i1; A names it has_offset).
- `air.sample_texture_Dd_grad.v4f32(tex, smp, coord, ddx, ddy, float min_lod_clamp, i1 true, <D x i32> zero_offset, i32 0)` -- same as A.
- B emits only: implicit LOD, explicit LOD, gradient, gradient + min-lod clamp, for D = 2, 3 float textures. No bias, no compare, no non-zero offset. Attributes: non-grad variant is `convergent`, grad variant is not (READ).
- B's descriptors (section 1) cover sampler selection; sampler pointer type is `%struct._sampler_t addrspace(2)*`.

### C: vertex-language/air intrinsic.go:518-524, README.md:247. READ. Confidence medium.
Emits `air.sample_texture_2d.v4f32` with 9 operands (tex, smp, <2 x float> coord, i1 true, <2 x i32> zero, i1 false, float 0, float 0, i32 0) = implicit-LOD, matches A/B; no semantic doc of bias/compare/gradient, no sampler_state support (only air.sampler argument metadata, metadata.go:125-126).

## Negative results (searched, nothing)
- SPIRV-Cross spirv_msl.hpp:115-235 (MSLConstexprSampler): enums are MSL SOURCE-level values (repeat=3, mirrored_repeat=4, clamp_to_border=2, clamp_to_zero=0, edge=1, compare NEVER..ALWAYS=0..7, border colours 0..2). They do NOT match the AIR bit values (observed repeat=2, mirror=3, border=4, compare default 8), so do not reuse them as bit values. They are useful only for the semantic set of fields (min/mag/mip filter, s/t/r address, compare func, border colour, anisotropy, lod clamp, coord mode). INFER: AIR bit packing is a different numbering.
- MoltenVK / Mesa upstream: use SPIRV-Cross -> MSL text; no AIR intrinsic or sampler-word code found (gh code search for air.sample in KhronosGroup/MoltenVK and mesa3d/mesa returned nothing). The AIR frontend in A is a patch set on top of Mesa/freedreno, not upstream Mesa.
- Apple public MSL spec text: not fetched in this pass; it documents sampler semantics (field names/defaults) but not AIR bit layout (INFER, not checked).

## Your dumps (/tmp/x231501/dump, /tmp/x230109/dump), strtab grep only
Intrinsic names present: air.sample_texture_2d.v4f16/v4f32 and air.sample_texture_3d.v4f16 only (strtab occurrence counts 58 and 12). No `_grad`, `sample_compare`, `sample_depth`, `gather` names in those 177+91 captures (grep of raw bytes; strtab is concatenated so counts are approximate). So only the plain form (offset, lod/bias flag, min_lod) can occur in those modules; gradient/compare handling is not required by them. Whether any call carries i1 lod_is_level=true, a non-zero offset or non-zero min_lod still needs your bitcode reader (no llvm-dis).

## Suggested implementation consequences (INFER, not from sources)
1. Decode the sampler word with A's layout; verify against B's 13 constants (both agree). Map: address 0 zero->clamp_to_border with transparent black (Vulkan CLAMP_TO_BORDER + TRANSPARENT_BLACK), 1 edge, 2 repeat, 3 mirror, 4 border; mip 0 -> maxLod 0.25-ish / mipmapMode nearest with LOD clamp (Vulkan has no "mip none": use minLod=maxLod=0 or maxLod=0.25), 1 nearest, 2 linear; bit 15 pixel -> unnormalizedCoordinates (needs edge/border address + no mips + no aniso; otherwise emulate by dividing by texture size); aniso field+1; lod_min/lod_max = half bits.
2. Sample operands: skip optional [has_offset, offset] (apply only when flag true and non-zero); lod_is_level true -> ImageOperandsLod, false & non-zero -> Bias, both with MinLod = min_lod when non-zero; _grad -> Grad (+MinLod). i8 status -> OpConstant 0 (replaces OpUndef).

# ADDENDUM (coordinator extra scope): bit comparison of the 4 words + dump trace
Method: llvm-dis 18.1.3 (now available) on /tmp/census_work/bc1/*.air.bc and bc2/*.air.bc (40+40 modules, all 80 disassemble; text in /tmp/smp_ll/*.ll). bc1 and bc2 give identical call statistics. Field names below are from source A/B (licence-clean per section 1); applying them to the dumps is INFER.

## Bit-by-bit
| word | low16 | binary (bits 15..0) |
|---|---|---|
| W1 0x807bff0000088a00 | 0x8a00 | 1000 1010 0000 0000 |
| W2 0x807bff0000088000 | 0x8000 | 1000 0000 0000 0000 |
| W3 0x807bff0000080092 | 0x0092 | 0000 0000 1001 0010 |
| W4 0x807bff0000080a49 | 0x0a49 | 0000 1010 0100 1001 |
Common to all four: bit 63; bits 24..55 (0x007bff00_00 -> lod_min 0x0000, lod_max 0x7bff = [0, 65504]); bit 19 (compare nibble = 8, the Apple default, per B); bits 20-23 = 0 (aniso 1); bits 56-62 = 0 (border/reduction default); bits 13-14 = 0 (mip filter none); bits 10 and 12 = 0.
Differing groups:
- bit 15: W1,W2 = 1; W3,W4 = 0. Field: coord mode (set = coord::pixel). INFER (A).
- bits 9 and 11 (together): W1,W4 = 1; W2,W3 = 0. Fields: mag and min filter (linear vs nearest). INFER (A), B corroborates the 0x0a00 = linear pattern.
- bits 0-8: W1,W2 = 000,000,000 (clamp_to_zero on s,t,r); W3 = 010,010,010 (repeat, 0x92); W4 = 001,001,001 (clamp_to_edge, 0x49). Fields: s/t/r address. READ in A and B.
Nothing differs in mip filter, compare, aniso, lod range, border: so none of the 4 shaders needs mips/LOD clamp/compare/aniso/border from the word. Vulkan note: W1/W2 = pixel coords + clamp_to_zero (unnormalized), the only combo needing special handling; W3 repeat+normalized and W4 edge+linear+normalized are plain.

## Which shader uses which word (all READ from the .ll)
- W1 (pixel, linear, clamp_to_zero): ShadowVerticalBlurRGBAFragment (05bcf7b433584329, a55b4e49ff1a39ed), ShadowHorizontalBlurFragment (5e4f61825be7332e, 628f41f215431462), ShadowCompositeFragment (1949ef372848ded0). All sample `texture2d<float> tex2D [[texture(0)]]` at coords derived from the stage_in float2 (`tc` / `tex`); blurs take 36 / 68 taps plus a buffer of per-tap offsets; one sampler global each. INFER: texel-space (pixel) coordinates, linear filtering so a blur tap between texels interpolates, zero outside so shadows fade to transparent at the edge (clamp_to_zero, not edge).
- W2 (pixel, nearest, clamp_to_zero): AlphaTextureFragment (680af02d9211a4bb): one sample of `texture2d` at the stage_in float2 `%1`, result mapped to (0,0,0,sample.w), i.e. a nearest-filtered alpha mask with zero outside. INFER: coordinate is in texels; no filtering wanted.
- W3 (normalized, nearest, repeat s/t/r) and W4 (normalized, linear, clamp_to_edge): both in A2Xghfc (35024e60e414abf4, the Uber shader, 8 call sites of 3d + 71 of 2d):
  * @__air_sampler_state.120 = W3: exactly ONE call, a 2d sample of entry argument 21 = `noise_tex` (metadata `i32 21 ... "noise_tex"`), coordinate = (value) / (buffer-loaded float scale) (A2Xghfc line ~3028-3032). INFER: dither/noise texture tiled with repeat, nearest so noise texels are not blended.
  * @__air_sampler_state.119 = W4: 9 calls: 8 `air.sample_texture_3d.v4f16` inside `sample_3d_lut` (texture arg `color_cube0`, a 3D colour LUT) and 1 `2d` sample inside `apply_color_opcode_noinline` (2D `color_trc*` gamma/transfer curve table; coordinate built from a looped index). INFER: LUT/curve lookups that must interpolate linearly and clamp at the ends.
  * The other 62 calls in A2Xghfc (`img_tex_*`) use the RUNTIME samplers `img_samp_0` / `img_samp_1` (entry args 30/31, air.sampler location 0/1) or functions' sampler params, not constexpr words.
- 7 of the 40 modules carry air.sampler_state: the 5 shadow modules, AlphaTexture, A2Xghfc (matches census X06).

## i8 status field: consumed or not
Across all 317 sample call sites (bc1) in the dumps (209 `2d.v4f32`, 100 `2d.v4f16`, 8 `3d.v4f16`; identical in bc2), every use of the call result is `extractvalue ... , 0`. No `extractvalue ..., 1`, no use of the whole struct. Status byte is NEVER consumed. => emitting OpUndef (or 0) for it is harmless for these modules; no dump evidence of a use. READ (script over the .ll, per-function scope).

## Non-default option operands in the dumps
Operand tail checked on all 317 calls: first i1 (has_offset) = true always, min_lod = 0.0 always, flags i32 = 0 always. No `_grad`, no `sample_compare`/`sample_depth`/`gather` anywhere (calls are only air.sample_texture_2d.v4f32 / 2d.v4f16 / 3d.v4f16). Non-default cases:
- Implicit LOD, plain: 269 sites.
- Bias with a variable value (i1 false, float %v): 28 sites, all `2d.v4f16`: 1-2 per Timg*/Tbds* family module (02cfce1d8cfa1792 TimgA2Xhfcu_Isrc, 0afcf78f70043a20, 14d36cc6140a23bd, 29e5e5cfe156af28, 29f9907dd5d89131, 4c68336ee725dfd6 (2), 6d9ac0fd69d28311, 7a85c45319c4b77f (2), 8522e3805ce37c32, 92e5d07bd4447d50, c39307e4b7602089, cdbc5cdbb3278950) and 14 in A2Xghfc. So a LOD bias (Metal bias()) really occurs: the translator must pass it (SPIR-V Bias image operand) rather than ignore it.
- Explicit level, constant 0.0 (i1 true, lod 0.0): 16 sites, all in 46d6a464a9d0afc2 downsample_8_frag_lph (`2d.v4f16`).
- Explicit level, variable (i1 true, float %v): 4 sites, all in A2Xghfc.
- Non-zero constant offset (i1 true, <2 x i32> constant): 11 sites, all `2d.v4f16` in A2Xghfc: (-1,0) (-1,1) (0,-1) (0,1) (0,2) (1,-1) (1,0) (1,1) (1,2) (2,0) (2,1); the rest (306) use zeroinitializer offset.
- Gradient: 0. Compare: 0. min_lod != 0: 0.
=> Of X04's operand list: bias, level and offset occur in the dumps (A2Xghfc, Timg*/Tbds*, downsample_8); gradient and compare do not. The order of these operands is documented in sources A and B (section 2), and the dumped IR matches that order (offset vector precedes the level flag, lod float precedes the zero min_lod float, trailing i32 0).
