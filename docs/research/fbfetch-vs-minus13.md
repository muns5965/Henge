# Does A2Xghfc use framebuffer fetch, and is the Khronos "Rasterization Order Attachment Access" sample related to the Adreno -13?

Read-only analysis. No repo changes, no device, no GPL sources. Tags: [V] verified from our own files, [I] inferred, [N] not checked.
Data: /tmp/x230109/dump/pipefail_50a6804e2e421a09/{fs.spv,vs.spv,state}, /tmp/x230109/logs, /tmp/x231501/{dump,logs}, /tmp/redfiles, /tmp/wt-samp2 (library worktree, branch research/g7-sampler2, src/*), docs/research/{pipe13-analysis,red-compare,census}.md.
Disassembly: spirv-dis of the stored fs/vs saved at /tmp/fs_a2x.dis and /tmp/vs_a2x.dis (20,194 lines; red-compare counts 20,189 instructions).

## Verdict (short)
The reviewer is right on the narrow point: A2Xghfc DOES do framebuffer fetch, and our failing pipeline state DOES use the exact feature set that sample exercises (dynamic-rendering local read plus the colour-blend rasterization-order flag). Every failing PSO in both device runs is a framebuffer-fetch PSO. But the correlation is perfectly confounded with "this one huge shader", so it is a live, top-ranked hypothesis, not a demonstrated cause. Several of the sample's pipeline-creation conditions are already satisfied by our state, so the sample gives us no obvious thing we are doing wrong; it gives us a checklist of what to vary.

## 1. SPIR-V facts (stored fs, 346,780 B, spirv-val vulkan1.3 passes [V])
- Capabilities [V]: Shader, Int64, Float16, VariablePointersStorageBuffer, Int8, Int16, InputAttachment, StorageBuffer16BitAccess, StorageBuffer8BitAccess. Extension: SPV_KHR_8bit_storage only. Memory model Logical GLSL450. Entry: Fragment "main", OriginUpperLeft only. No ExecutionMode for rasterization order (the EXT has none; it is purely a pipeline/subpass flag).
- Framebuffer fetch [V]: `%7398 = OpTypeImage %float SubpassData 0 0 0 2 Unknown` (Sampled=2); two UniformConstant variables %34 (DescriptorSet 0, Binding 40, InputAttachmentIndex 0) and %35 (Binding 41, InputAttachmentIndex 1). Exactly 2 OpImageRead, both in the entry function, executed unconditionally at its start (lines 12156/12159, coordinate = OpConstantNull v2int). Results go through OpFConvert to v4half and are stored into Function variables that are used later (lines 12229, 12250, 12437, 12447, 14167, 14177, 20156, 20164), so the reads are live, not dead.
- Outputs [V]: two Output v4float variables, Location 0 and Location 1 (no Index decoration, not dual-source); stored at the end (lines 20183, 20186). Inputs: gl_FragCoord, gl_PointCoord, Location 2..6.
- So the shader reads colour attachments 0 and 1 and writes colour attachments 0 and 1 in the same invocation: the classic read-modify-write that needs either a barrier or the rasterization-order flag.
- Translator origin [V]: src/air2spv/translate_function.c:318-319, 2636-2643, 2915-2918: an AIR fragment input keyed "air.render_target:<n>" (Metal [[color(n)]]) becomes an input attachment (SubpassData, InputAttachmentIndex n, set 0, binding 24+n before offset_bindings; 40/41 in the device module).
- Other counts [V, re-counted]: OpPtrAccessChain 70, OpBitwiseOr 4, OpUndef 13, OpSpecConstantOp 0, OpForwardPointer 0, OpSwitch 14, OpLoopMerge 14, OpPhi 0, OpSampledImage 72, OpImageSampleImplicitLod 72, OpDPdx 5 / OpDPdy 5 / OpFwidth 16, no OpKill/demote/barriers/atomics. 12 UniformConstant 2D images + 3 samplers (+2 SubpassData) in the layout; 13 storage buffers.
- VS [V]: Capabilities Shader, Int64, Int8, Float16, StorageBuffer16BitAccess, StorageBuffer8BitAccess; Vertex, no input attachment, 10,268 B. Nothing fetch-related.
- Important caveat on the "control" A2Xghfc (red-compare.md) [V]: the 35024e60_A2Xghfc "BUILT (control)" module (346,656 B, no InputAttachment) comes from the dt2 offline translation, not from a device build; its BUILT status is inferred from absence in a fail line. It is the same function without the 124 extra bytes of fbfetch declarations. There is NO device evidence that A2Xghfc without framebuffer fetch builds on the Adreno. [N: whether any older device run built it]

## 2. Our pipeline creation, device, and logs
### Pipeline state (src/vulkan/pipeline_build.c, arm64_exec_run.c) [V]
- Dynamic rendering, no VkRenderPass: VkPipelineRenderingCreateInfo with colorAttachmentCount=2, formats 44 (B8G8R8A8_UNORM)+97 (R16G16B16A16_SFLOAT) (or 97+97), depth/stencil UNDEFINED. The pNext of that struct is only a diagnostic hook (diag_gpci_pnext, NULL in production).
- We never chain VkRenderingAttachmentLocationInfo or VkRenderingInputAttachmentIndexInfo in production (only in the opt-in matrix, pipeline_matrix.c rows M5a/b/c). Per the Khronos local-read proposal (fetched) the defaults are identity (input index i = colour attachment i; location i = i) and the structs are not required. Spec VUID-VkGraphicsPipelineCreateInfo-renderPass-09652 (from Khronos validusage.json, spec 1.4.369): with no VkRenderingInputAttachmentIndexInfo the shader must not use an InputAttachmentIndex >= colorAttachmentCount; ours are 0,1 < 2, OK.
- Colour blend: VkPipelineColorBlendStateCreateInfo.flags = 0x1 (RASTERIZATION_ORDER_ATTACHMENT_ACCESS bit; code comment says EXT bit == ARM bit value; the numeric value was not re-checked against the header here [N]) set iff the PSO has an INPUT binding and the device has the feature (arm64_exec_run.c:477). attachmentCount 2, independentBlend=1, blend disabled in the saved bundle, write masks 0xf/0xf. No depth/stencil-state flag.
- Dynamic state: viewport, scissor, cull mode, front face, depth bias (1000267000/1 are CULL_MODE/FRONT_FACE). Topology triangle list, no vertex input (VS pulls from buffers).
- Layout [V, state file]: 1 set, 30 bindings: 13 STORAGE_BUFFER (VS|FS), 12 SAMPLED_IMAGE, 3 SAMPLER, bindings 40 and 41 = type 10 INPUT_ATTACHMENT, stageFlags 16 (FRAGMENT only, as required). No push constants, no spec constants.
- No legacy render pass, no subpass input attachments, no VkSubpassDescription flags in production; those exist only in the diagnostic matrix rows M6a/M6b (pipeline_matrix.c:168).
- Draw-time (not relevant to the -13, which happens in vkCreateGraphicsPipelines): descriptors use layout 1000232000 RENDERING_LOCAL_READ (arm64_exec_run.c:654), attachments same layout, a BY_REGION memory barrier between draws (:745-750). g7-gpu-pipe says fbfetch draws=0 in the failing runs: the failure is before any fetch draw.

### Device creation (src/vulkan/instance.c) [V]
- Enabled device extensions (from the bundle `dev.extensions`): VK_KHR_dynamic_rendering, VK_KHR_dynamic_rendering_local_read, VK_EXT_rasterization_order_attachment_access. VK_EXT_shader_object and VK_EXT_extended_dynamic_state3 are absent on this device (log lines 12-13).
- Enabled features: shaderInt64/16/8, shaderFloat16, storageBuffer8/16BitAccess (uniformAndStorageBuffer*/storagePush*/storageInputOutput16 = 0), variablePointers + variablePointersStorageBuffer, descriptorIndexing, bufferDeviceAddress, dynamicRendering, synchronization2, subgroupSizeControl, dynamicRenderingLocalRead=1, rasterizationOrderColorAttachmentAccess=1 (queried also: Depth=1, Stencil=1, not enabled). Env switches LAGFX_VK_NO_LOCAL_READ / LAGFX_VK_NO_RASTER_ORDER unset in the runs.
- Device facts from the bundle/log [V]: Adreno (TM) 840, apiVersion 1.4.295 (4210983), vendor 0x5143 (20803), deviceID 1141180977, driverVersion 2150932499 = 0x8034A013, driver name "Qualcomm Technologies Inc. Adreno Vulkan Driver", build 089d643e2a, compiler E031.50.19.27, subgroup size 64 fixed. Core 1.4 reports local read as core (core1.4=1) and the extension is also present (ext=1).
- The full list of extensions Adreno 840 REPORTS is not in any saved log; only the three enabled ones are recorded [V, grep of /tmp/x230109, /tmp/x231501, /tmp/stall, /tmp/zx logs].

### The failures are exactly the fbfetch pipelines [V + I]
- Counter semantics [V]: arm64_pipeline.c:578 increments fbfetch_pipes once per fragment INPUT_ATTACHMENT binding, so pipes/2 = number of fbfetch PSOs (A2Xghfc has 2).
- x230109 run: fbfetch pipes=4 -> 2 PSOs; build_failed=2. x231501 (R1) final: fbfetch pipes=42 -> 21 PSOs; build_failed=21 (log summary also says 21 failures; only 8 distinct bundles are kept). So in both runs every fbfetch PSO failed and, by the count, no non-fbfetch PSO failed (built=34 of 37 / built=16 of 38 are the rest) [I from counter arithmetic].
- Variation across the 8 R1 bundles + 2 x230109 bundles [V]: 4 different vertex stages (hashes 1a8fd2ce, 8a5734ee, 5fbcd5a0, 22942f5e), colour formats 44+97 and 97+97, blend on and off, write mask of colour 1 = 0xf or 0. All have n_color=2, raster_order_access=1, same FS function. So the failure does not depend on VS, format pair, or blend, which weakens "interface linkage" and "blend/format" as causes.
- Never any PSO with framebuffer fetch that built [I]: no evidence of one. [N: whether any fbfetch PSO with a SMALL fragment shader exists in our data; none seen: every IA module in all dumps is A2Xghfc (hash set 05e8e5..., 6a6414..., 8ec1ed..., be9969..., ce5c70..., size 346,780 B).]

## 3. The sample's conditions that do and do not apply
Source of conditions: docs.vulkan.org samples dynamic_rendering_local_read page (fetched), Khronos proposal pages for VK_EXT_rasterization_order_attachment_access and VK_KHR_dynamic_rendering_local_read (fetched), Khronos validusage.json (spec 1.4.369, downloaded to /tmp/vu_dl, outside any repo). The docs.vulkan.org rasterization_order_attachment_access sample page itself could not be fetched (client_error), so that sample's own text is [N]; no sample code was fetched or copied.

| Condition | Source | Ours |
|---|---|---|
| Enable local-read extension + feature | local_read sample/proposal | yes [V] |
| Enable rasterizationOrderColorAttachmentAccess; flag only valid with it | VUID ...-06465 | yes [V] |
| Pipeline created from VkPipelineRenderingCreateInfo, no render pass | local_read sample | yes [V] |
| EXT flag usable with dynamic rendering when local read is enabled | local_read proposal (the older EXT proposal says it is render-pass scoped, which predates local read) | we do this; VUIDs 06061 and 06482 are conditioned on dynamicRenderingLocalRead NOT enabled [V from validusage.json], so the lavapipe-layer messages 06061/06482 in pipe13-analysis.md are layer-age artefacts, now confirmed against the spec text |
| Input-attachment index mapping: default identity unless VkRenderingInputAttachmentIndexInfo; index < colorAttachmentCount | proposal, VUID 09652 | default, 0/1 < 2 [V] |
| Output location mapping default identity | proposal | default [V] |
| Mapping/location must match between pipeline and command-buffer state at draw | proposal | draw-time only; no structs on either side |
| Input attachment descriptors, fragment stage only | sample/spec | bindings 40/41, FRAGMENT [V] |
| Layout RENDERING_LOCAL_READ (or GENERAL) for attachments and descriptors | sample | draw-time [V code] |
| Barriers inside rendering need BY_REGION | sample | draw-time [V code] |
| Shader: subpassInput/subpassLoad (SubpassData, OpImageRead at (0,0)) | sample | yes [V] |
| Blend state / attachment formats | not specified by the sample beyond 1:1 mapping | blend off in main bundle; formats 44/97 mandatory colour+blend formats per reviewer's reading of the Formats chapter [N: not re-read here] |

Conclusion on (3): the sample is a valid minimal reference for what a conforming local-read + rasterization-order pipeline looks like, and our pipeline matches it on every creation-time condition I can check. It is NOT a pointer to a missing structure. What it cannot cover is a 346 KB fp16/int64/variable-pointer shader. Whatever the Adreno rejects is therefore either (a) a driver-specific requirement on how fbfetch pipelines are declared, or (b) something in the shader that the fbfetch pipeline path exposes.

## 4. Ranked hypotheses still alive for -13 (Adreno 840, A2Xghfc)
Facts used: 0 OpSpecConstantOp, 0 OpForwardPointer, 70 OpPtrAccessChain on StorageBuffer, 4 OpBitwiseOr, 13 cap-bail OpUndef, plus section 2 (100% of fbfetch PSOs fail, VS/format/blend independent). Row names are those of the existing ladder (pipeline_reduce.c) and matrix (pipeline_matrix.c). Everything below is hypothesis [I]; none is device-verified.

1. H1 Framebuffer-fetch pipeline declaration (InputAttachment + local read + rasterization-order flag) rejected by the Adreno compiler/driver. Perfectly confounded with the shader. Sub-variants: 1a the flag itself (never decoupled from IA in production); 1b missing explicit VkRenderingInputAttachmentIndexInfo/LocationInfo; 1c input-attachment binding numbers 40/41 and layout shape (ANGLE-style contiguous block, own set); 1d dynamic-rendering path vs legacy render pass; 1e feedback of the same pair in a 2-attachment, different-format MRT.
   - Discriminators: IADROP (OpImageRead->null and the SubpassData variables removed, full body, flag still set) builds => fbfetch declaration/IA is the trigger; IADROP fails => H1 dead for the shader body. READ (read nulled, variables kept) separates the OpImageRead from the declaration. IFC (minimal FS with IA, write nulls) fails => state/IA alone suffices (strongest result: then the 346 KB body is irrelevant); IFC builds and IFE (minimal, no IA) builds => the body matters. M1 (flag off) builds => the flag; M5a/b/c => explicit mapping; M3a/b, M4a => binding layout; M6a/b => legacy render pass route.
2. H2 Size/complexity limit or compiler crash on the entry function (346 KB, 1,944 Function variables in one 9,994-line function, 723 labels, 56 functions, 761 64-bit dynamic indices, 188 FConvert). Not excluded because no device evidence that the non-fbfetch A2Xghfc builds (section 1 caveat). A small shader with fbfetch is the missing control.
   - Discriminators: IFC/IFD/IFE (minimal); HALL, HA/HB, HSMALL, HCF (helpers stubbed); LP/LP-E/LP-H, SW/SW-E/SW-H/U1 (control flow); offline red.local-single-block (338 KB), red.u3 (85 KB), red.u2 (210 KB), red.scalar, red.O (9.5 MB, expected bigger not smaller). Read as size ladder: the smallest failing module and the largest building module bracket the limit; if IADROP fails and u3 builds, it is size/structure, not fbfetch.
3. H3 VariablePointersStorageBuffer with 70 OpPtrAccessChain on StorageBuffer pointers passed as function parameters (A2Xghfc is the only module with this and with IA; a control with it exists but its device status is unknown). 
   - Discriminators: red.inline (504 KB, removes pointer params if all calls inline), HALL (helper bodies stubbed so pointer-param calls vanish), HSMP; R0 vs R0V unaffected.
4. H4 Interface linkage (Cemu #2029 style) or stage-interface rejection: FS inputs Location 2..6 vs VS outputs. Weakened: 4 different VS fail alike, and real-pair types match by reading [V pipe13-analysis]. 
   - Discriminators: R0 (stub VS) vs R0V (real VS): R0 builds & R0V fails => linkage; both fail => FS+state. IFA/IFB (no fragment stage) builds => the VS and layout are fine (expected). IFF (generated VS writing every FS input) builds => linkage.
5. H5 OpUndef, in particular the 13 cap-bail returns that reach outputs/control/address (X03), or struct/pointer undef (14/27 modules have struct undef and some build, so undef alone is not the answer): UN, US, UP(n/a), UU-CALL/CCOMP/CINS/INSDYN/SHUF/STORE, M9g..M9k.
   - Discriminator: a lone-undef row that flips the result while IADROP does not; or flips only with fbfetch. Low-medium.
6. H6 Large static SSBO array type (struct { half[32768], half[32768] }, 128 KiB) or runtime-array handling: S6 row. Low.
7. H7 64-bit dynamic vector indices / Int64 + dynamic access (761 sites, controls have these too): DYN-C, DYN-S, DYN-S1, DYN-S4; red.* "e" variant. Low.
8. H8 Derivatives (5 OpDPdx/DPdy, 16 OpFwidth) or implicit-lod sampling (72) in non-uniform control flow inside loops/switches: DERIV, LOD0, SMPN*. Low (also in building controls).
9. H9 OpBitwiseOr x4: no row targets it; nothing in public reports I know of ties it to -13 [N: not searched this pass]. Very low; keep only to be exhaustive, a 4-instruction feature that cannot be the only difference unless one of them has an unusual type (check their operand types before spending device time).
10. H10 A cache/timing/ordering effect rather than the module: FL-NOOPT (DISABLE_OPTIMIZATION flag), as-is rebuild x3, fresh process. Low; pipe13 lavapipe run reproduces nothing, which says nothing about Adreno.

Reading the first device results (R0 = original FS + stub VS; R0V = original pair with the real VS), assuming the failing state is kept in both:
- R0 FAIL and R0V FAIL: linkage excluded; FS body or fbfetch state. Continue with IADROP / IFC.
- R0 BUILDS and R0V FAILS: linkage/VS interaction (H4). R0 FAILS and R0V BUILDS: unexpected (stub VS lacking outputs); would also point to linkage rules.
- R0 BUILDS and R0V BUILDS: failure is not reproducible in the reduce harness (state or process difference vs the app path); check the app-path state (feature enables, pipeline cache, isolation), not the shader.
- IADROP BUILDS (with R0V failing): H1 (fbfetch declaration) is the cause or at least a necessary trigger; then M1, M5*, M3*, M4a, M6* pick the sub-variant.
- IADROP FAILS: the shader body or its other capabilities (H2/H3/H5..H8); then the HALL / red.local-single-block / red.u3 / IFC ordering above.
- IFC (minimal FS with IA, real VS, same state) FAILS: H1 stands alone; the body is exonerated.
- IFC BUILDS and IFE BUILDS and R0V FAILS: H2/H3 (something in the body; bisect by HALL, HA/HB, LP, SW, U1).

## 5. What I did NOT check
- The reviewer's sample page for rasterization_order_attachment_access (fetch failed); its Vulkan-Samples code (Apache-2.0, intentionally not fetched or copied).
- Whether any older device run built A2Xghfc without framebuffer fetch, or any fbfetch PSO from a different shader.
- Adreno 840's complete extension list, Qualcomm release notes/known issues.
- The numeric value of the colour-blend flag bit in a Vulkan header (code comment says the EXT and ARM values coincide).
- Format support (COLOR_ATTACHMENT + BLEND, INPUT_ATTACHMENT-capable usage) of formats 44 and 97 on this device; no log line records it.
- Operand types of the 4 OpBitwiseOr.

## Candidate facts for memory (from this session)
- Failing PSO set equals the framebuffer-fetch PSO set in both device runs (2 of 2 and 21 of 21; counter fbfetch_pipes counts bindings, not PSOs); source: library logs /tmp/x230109, /tmp/x231501 plus arm64_pipeline.c:578.
- red-compare.md control A2Xghfc (35024e60) is an offline translation, not a device build.
