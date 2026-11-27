# False-skip / unnormalized-sampler usage gaps (a)-(c): analysis from OUR data only

Read-only. Nothing in the repos was changed, built, committed or booted. Library: /tmp/wt-integ2 (HEAD 20c3624).
Scratch scripts: /tmp/fsa/a.py and /tmp/fsa/out/*.txt (spirv-dis output). No GPL sources opened.

Tags: [V] verified from data or code, [I] inferred, [N] not checked.

## 0. Data actually available

- Only TWO distinct real runs carry the unnorm counters:
  - /tmp/x231501 (run 20261126-231501-626). Log 26 MB, capture dump present (256 captured draws, 179 cap_samp rows, 903 cap_img rows).
  - /tmp/x230109 (run 20261126-230109-397). Log 6.5 MB, NO capture (cap[draws=0]). /tmp/stall/zip_on/logs is byte-identical to x230109 (cmp).
- Both runs were produced by henge 456244c8 / lagfx 779802a, not by 20c3624. The (a)-(c) code paths, the counters `ext_unnorm_lod0` / `ext_unnorm_skipped` and the alt-shader logic already exist in that build (counters printed in the g7-gpu-ext line). The newer counters (tex3d_binds, cxs_*, rt_addr_unknown) are not in these logs. [V]
- Other g7_lagfx logs found on disk (/tmp/zx, /tmp/fd1203*, /tmp/files*): no unnorm counters, so no information. [V]
- /tmp/lagfx-cap-* (1224), lagfx-dump-*, lagfx-spv-*, lagfx-mapped-*: unit-test or tool temp output (synthetic). No cap_samp rows, no real-run draws. They give no real-run data. [V]

## 1. How often do unnormalized runtime samplers occur?

Run-wide counters (g7-gpu-ext, last line):

| run | draws | draws_skipped | sampler objs (samplers=) | unnorm_applied (`ext_samp_unnorm`, per sampler OBJECT) | unnorm_lod0 (`ext_unnorm_lod0`, per draw using the alt shader) | unnorm_skipped |
|---|---|---|---|---|---|---|
| x231501 | 437 | 296 | 6 | 2 | 30 | 0 |
| x230109 | 823 | 165 | 6 | 2 | 32 | 0 |

- So 2 of the 6 runtime sampler objects are unnormalized (same ratio as the "two of six" comment in arm64_sampler.c). 30 of 141 executed draws (x231501) and 32 of 658 executed draws (x230109) go through the explicit-LOD-0 alt fragment shader. `ext_unnorm_lod0` is incremented before the pipeline is built, so it is an upper bound on draws that really ran. [V]
- Capture of x231501 (cap_samp, cap_bind, cap_draw): 18 of 179 sampler snapshots have unnorm=1. They are only 2 distinct states: nearest/nearest (9) and linear/linear (9). Both have U=V=W=2 (clamp-to-edge as captured), mip off, lod 0/0, no compare, no aniso. That matches decode of words 0x04000000 and 0x04000005 (bit0 = min linear, bit2 = mag linear; I did not read the raw words, only the decoded fields) [I].
- 18 of the 256 captured draws bind an unnormalized sampler (always fragment sampler slot 0, always exactly one 2D image, d=1, mips=1). They belong to exactly two pipelines, both `tag=1` draws:
  - pipe_hash 42d8d4b80b4475e4, fs blob 15 (1868 B): 9 draws, 6 vertices, passes 960x24 / 88x48 (1920x48 image strips), colour fmt 44, source image mtl fmt 80.
  - pipe_hash f7986eab1ec2b568, fs blob 34 (1156 B): 9 draws, 24 vertices, passes 1920x1080 with 2 colour attachments (fmt 97), source image 1920x1080 mtl fmt 115 (RGBA16F).
- All 18 captured draws have `skip=-` (executed). That means the alt pipelines for these two shaders were built and drawn. [V]
- The whole-run 30/32 draws cannot be assigned to shaders beyond the captured 18. That the other 12 use the same two pipelines is [I] (only 2 sampler objects are unnormalized and both appear in the capture).

## 2. Draws skipped because fs_alt was NULL

- The exact site is arm64_exec_run.c:735: `if (alt && !r->pipe->src.alt_fragment_shader) { CNT(p).ext_unnorm_skipped++; CNT(p).draws_skipped++; return false; }`. There is no log text at that line. The counter is `ext_unnorm_skipped`, printed as `unnorm_skipped=` in the g7-gpu-ext line (diag.c:189/195), also the capture skip reason string "unnorm_skipped" (cap_skip_reason) and the draw-diag column "unnorm_skipped". [V]
- IMPORTANT: it is ONE counter for two different causes: rewriter returned -1 (Bias/Grad/MinLod/malformed: a real, intended skip) and rewriter returned 0 (no implicit-LOD sample: possible false skip, case (c)), plus fs_alt creation failure. The counter cannot tell them apart. [V from code]
- Values: `unnorm_skipped=0` in all 17 g7-gpu-ext lines of x231501 and all 17 of x230109. No capture row has skip=unnorm_skipped. -> 0 false skips, 0 intended skips in real runs. [V]
- Accounting of all skipped draws (so nothing hides):
  - x231501: 296 = no_pipeline 254 + desc unbound 33 (all tex, texwhy not_mirrored=30, slot_unbound=3) + desc unaligned 9 + unnorm 0. [V]
  - x230109: 165 = no_pipeline 33 + desc unbound 132 (buf=15, tex=117, not_mirrored=117) + 0. [V]
- Translated fragment shaders in the capture: all 6 distinct fs blobs have either no image sampling (blobs 5, 11) or exactly one OpImageSampleImplicitLod with NO image operands (blobs 15, 32, 34, 63). So the rewriter returns 1 for every captured shader that samples; case (c) (rc 0) is not hit. [V] The big uber fragment shader (346780 B) has 72 OpImageSampleImplicitLod, all without operands. [V]
- The 8 logged pipeline build failures in x231501 (VK_ERROR_UNKNOWN -13, fs entry A2Xghfc, 346780 B, 72 ImplicitLod, 0 ExplicitLod, pipefail_*/fs.spv) are the ORIGINAL fragment shader, not the alt variant (alt would have 0 ImplicitLod). So those failures are not caused by the unnormalized rewrite. 13 further failure keys (build_failed=21) were not logged ("more than 8 distinct failure keys"): that none is an alt variant is [I] (alt needs an unnormalized sampler and the unnormalized draws all executed).

## 3. Unnormalized sampler + 3D binding, or ConstOffset

- 3D: all 903 cap_img rows have d=1, layers=1, mips=1. All 6 captured fragment shaders have only `OpTypeImage 2D` (the uber shader additionally SubpassData). The 18 unnormalized draws have exactly one 2D image each. No Dim3D binding with an unnormalized sampler in the data. [V for the capture]
- Not covered: the whole run beyond the capture, because `tex3d_binds` and `tex_dim_mismatch` are not in these logs. A 3D sample function exists in one library (lib_35024e60e414abf4.bin, air.sample_texture_3d.v4f16, used by many pipelines through function ref 31), but the translated SPIR-V has no 3D image type, and the trace shows only `call: air.sample_texture_2d` (1724 and 373 calls), no `sample_texture_3d` call. [V that no 3D call was translated in these two runs; the trace text is static ImplicitLod text, so it cannot show explicit-LOD or offset forms]
- ConstOffset: no OpImageSample* in any captured fragment shader has an image operand at all (blobs 15/32/34/63: no operands; uber: no operands). No `sample_texture opts:` log line (the translator logs offset=... there) appears in either log. So no sample with ConstOffset exists in these runs, bound to an unnormalized sampler or otherwise. [V]
- The unnormalized draws use no Bias/Grad/Lod/offset. [V]

## 4. Exact conditions and minimal safe fixes (described only)

### (a) ConstOffset kept by `lagfx_spv_explicit_lod0`
- Code: src/air2spv/spv_reflect.c:278 accepts `wc == 7 && image-operand mask == 0x8` (ImplicitLod with ConstOffset only), and :301 emits `OpImageSampleExplicitLod` with mask 0xA (Lod | ConstOffset), operands Lod then the original offset id. The operand order is correct. The translator does emit this form: translate_function.c:5208 (`use_off`, 2D `sample_const_ivec2`, 3D ivec3). Every other form (Bias, MinLod, Grad, Offset 0x10, ConstOffsets 0x20, ConstOffset+Bias) returns -1.
- Condition for the gap: fragment shader samples with a Metal offset AND a fragment sampler of the pipeline is unnormalized. Spec (as read in the review) forbids offsets for unnormalized samplers (poison value). Data: never happens.
- Minimal safe fix, option A1: delete the `wc == 7` branch at :278 and the `else` branch at :301 so such shaders return -1 (draw skipped and counted, same as Bias). Update the comment in spv_reflect.h:106. About 3 lines. Risk: a shader that today runs "best effort" (drivers often just ignore the offset or honour it) becomes a skipped draw. Zero draws in our data are affected, so no change for current runs.
- Option A2 (more faithful): fold the offset into the coordinate (texel units, so coord + float(offset) is equivalent for clamp-to-edge). Needs a type map for the coordinate's vector type, two new ids (OpConvertSToF, OpFAdd) and a bound update. Larger code, more risk of malformed SPIR-V. Only worth it if a real shader needs it.

### (b) Dim3D image with an unnormalized sampler
- Code: arm64_exec_run.c:679 chooses `lagfx_a64_res_sample_view3d` (arm64_resources.c:1401, VK_IMAGE_VIEW_TYPE_3D) when `s->bind[i].dim3`. prepare_pipeline (:730-735) looks only at `r->fsamp[mi]->ci.unnormalizedCoordinates` of the pipeline's stage-1 sampler bindings. Nothing connects the two.
- Why a fix is not trivial: at descriptor level image and sampler bindings are separate (Metal style), the pairing exists only in the SPIR-V (OpSampledImage). A shader may legally use one pixel-coordinate sampler for a 2D texture and a normalized sampler for a 3D LUT in the same draw. A blunt rule "any dim3 image + any unnormalized sampler in the pipeline -> skip" would then skip a legal draw (a new false skip). Also the alt shader turns ALL implicit samples of that shader (including the 3D LUT one) into LOD 0 (existing side effect of the whole-module rewrite).
- Minimal safe fix: in prepare_pipeline, when `alt` is set, loop over the pipeline binds; if a stage-1 IMAGE bind has `dim3`, count a NEW counter (e.g. `unnorm_3d`) and skip the draw. About 4 lines. First step should be count-only (do not skip), so the first real occurrence is visible; that has no behaviour risk. The exact fix needs the OpSampledImage pairing.
- Data: no occurrence in the capture; run-wide not countable with these logs.

### (c) rc 0 collapsed into "fs_alt NULL" -> skip
- Code: arm64_pipeline.c:589 creates fs_alt only if `fsamp && lagfx_spv_explicit_lod0(...) == 1`. rc 0 means "no OP 87 in the module" and rc -1 "cannot rewrite"; both leave `alt_fragment_shader` NULL. exec_run.c:735 then skips the draw whenever an unnormalized fragment sampler is bound.
- False-skip case: pipeline has a fragment sampler binding but its shader has no OpImageSampleImplicitLod, e.g. only explicit-LOD samples (the translator emits OpImageSampleExplicitLod for `level(n)`: translate_function.c:5198) or fetch/read. For fetch/read the sampler is unused (harmless); for `level(0)` the shader is already legal.
- Minimal safe fix: record rc in the pipeline (a bool such as `alt_failed` set when rc == -1 or module creation failed). In prepare_pipeline: `if (alt && !alt_fragment_shader) { if (src.alt_failed) skip as today; else alt = 0; /* nothing to rewrite, use the normal pipeline */ }`. About 4 lines in two files, and no change to the pipeline cache key. Also split the counter (`unnorm_skipped_rc_neg` vs `unnorm_lod0_unchanged`).
- Risk: draws that are skipped today would now run with an unnormalized sampler and the original shader. If that shader uses explicit Lod != 0, Dref, Gather or Proj with the pixel sampler, the Vulkan result is poison (not a skip). I did not verify the spec for explicit Lod != 0, so the safe form is to allow rc 0 only when the module has no OpImageSample*/Gather at all, or whose ExplicitLod operand is a constant 0.0. [I; not verified in the spec]
- Data: not observed (counter 0; all captured samplers' shaders have ImplicitLod).

## 5. Conclusion on the missing Apple logo / macOS screen

- (a) ConstOffset: no evidence. No offset sample in any captured or logged shader of these runs.
- (b) 3D + unnormalized: no evidence in the capture (all 2D, d=1). Not measurable run-wide because the tex3d counters are not in these logs.
- (c) false skip: no evidence. `unnorm_skipped=0` in both runs (all 17 diag lines each); the captured unnormalized draws all ran.
- The unnormalized path itself is exercised in real runs (30 and 32 draws, including two 1920x1080 RGBA16F full-frame passes) and these draws executed, so it cannot be the cause of draws missing in these runs.
- Where the skipped draws in these runs really come from (measured): pipeline build failure -13 of the uber fragment shader (A2Xghfc, 346780 B, SubpassData + 72 samples, original not alt): 254 of 296 skipped draws in x231501, and texture-not-mirrored / unaligned descriptors (33 + 9 in x231501, 132 in x230109). That is where to look first for the missing screen. This is a pointer from the counters, not a root-cause claim.

Not checked: raw sampler words 0x04000000/0x04000005 in the blobs (only the decoded cap_samp fields); sample call forms inside uncaptured shaders; spec text on explicit Lod != 0 / Dref / Gather with unnormalized samplers; any run newer than 231501.
