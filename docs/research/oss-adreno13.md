# Adreno 840 vkCreateGraphicsPipelines -13: public-case research (web only)

Date 2026-11-27. Sources fetched via a summarizing tool (second-hand, treated as untrusted); not independently verified.

## Bottom line
- FACT: No public report found that matches our combination (A8xx + dynamic rendering local read + rasterization-order attachment access + huge SPIR-V). No Qualcomm-confirmed root cause for any similar -13 case. Nothing found in Mesa/turnip docs about A8xx local_read limits (freedreno doc page does not mention input attachments, local read, or a8xx).
- FACT: Pattern across public cases: Adreno compiler fails SPIR-V that is valid per validators; vendor-side answer is "send us a repro"; community workarounds are shader-rewrites or dropping optimizer passes. Cause is typically a backend compiler bug, not an API usage error.
- INFERENCE: Our failure is most likely a compiler-backend failure (size/feature combination), so bisecting the shader is likely to be more productive than API variants A-F.

## Cases
1. Qualcomm forum comment/20502 (https://developer.qualcomm.com/comment/20502) - Adreno 650 (Quest 2), driver 0667.0, compiler E031.40.18.00. vkCreateGraphicsPipelines returned -13 after shader edits; log said "Failed to link shaders". Cause unknown; simplified shaders worked. No fix stated. Applicable: symptom/log text match (check logcat for same string). Confidence low-medium as hint only.
2. Qualcomm forum 71271 (https://developer.qualcomm.com/forum/qdn-forums/software/adreno-gpu-sdk/71271) - Adreno 640, compiler EV031.32.02.12, Unity, -13, "Failed to link shaders" with internal assertion about a 32-bit integer multiply pseudo-op. Poster worked around by restructuring conditionals (turning ifs into arithmetic/select). No vendor reply. Applicable: shows linking-stage backend asserts triggered by control-flow shape; relevant to our 14 OpSwitch + int ops. Confidence low-medium.
3. Qualcomm forum comment/22767 (https://developer.qualcomm.com/comment/22767) - Adreno 750, compute pipeline VK_ERROR_UNKNOWN; semantically equal SPIR-V with different mask/or ordering succeeded. Workaround: rewrite expression. Shows even trivial integer-expression shape can trip compiler. Applicable: suggests spirv-opt/DXC/glslang output differences matter. Confidence low.
4. Qualcomm comment/18842 (https://developer.qualcomm.com/comment/18842) - Adreno, Android 11, driver build Dec 2020; VK_ERROR_UNKNOWN for SPIR-V tessellation shaders; compiler assumed GLSL-derived gl_PerVertex structure. Qualcomm said fix in future drivers; early A7xx OEM drivers cannot be updated directly. Lesson (FACT per post): Adreno's SPIR-V front end historically expects glslang-shaped input; unusual SPIR-V shapes can fail. Applicable: if our SPIR-V comes from a non-glslang producer (Tint/DXC/SPIRV-Cross/custom), try normalizing. Confidence medium-low.
5. Qualcomm comment/13638 (https://developer.qualcomm.com/comment/13638) - Adreno 530/540/630: "Shader compilation failed", pipeline returned VK_INCOMPLETE; cause: spirv-opt output; workaround: skip spirv-opt or disable individual passes found by bisect. Applicable: if we ran spirv-opt, try without; if we did not, try specific passes (legalization/inlining/DCE) to restructure. Confidence medium as an experiment.
6. Qualcomm comment/11420 (https://developer.qualcomm.com/comment/11420) - Adreno 505, pipeline with input attachments returned VK_INCOMPLETE; later comment says a driver update fixed it on another device. Old (2018). Shows input-attachment shaders have been a driver weak spot. Applicable: weakly. Confidence low.
7. Qualcomm comment/18799 (https://developer.qualcomm.com/comment/18799) - Adreno 610/615/618: explicit float16_t shaders failed; suspected missing float-controls/rounding support; workaround: fp32 fallback. Applicable: our Float16 capability; try removing Float16/Int16 usage or capability declarations. Confidence low-medium.
8. Qualcomm comment/18835 (https://developer.qualcomm.com/comment/18835) - Adreno, compiler EV031.29.00.16: "Invalid SPIR-V module: ID bound" internal compiler error, no resolution; Qualcomm asked for the shader. Shows that the driver re-parses SPIR-V with its own limits (ID bound). Applicable: our 346 KB / 20k-instruction module likely has a large ID bound; check bound and try compacting IDs (spirv-opt --compact-ids). Confidence low-medium (INFERENCE).
9. PPSSPP Vulkan driver-bugs page (https://www.ppsspp.org/docs/articles/vulkan-driver-bugs) - only Adreno 5xx entries (stencil/discard, color mask). Not applicable; no A8xx or pipeline-creation entries.
10. Unreal forum (https://forums.unrealengine.com/t/android-pso/2554557) - Epic reports PSO precompile failures on many Adreno 7xx driver builds (2022-2024), reported to Qualcomm, no cause or workaround. Shows driver-version-specific PSO failures exist and vendor escalation is the only path. Applicable as precedent. Confidence medium that it is the same class.
11. Mesa turnip MR 15106 (https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/15106) - turnip implements rasterization_order_attachment_access by setting a single-primitive-mode GRAS register, forces late Z for depth/stencil feedback, forces GMEM; motivated by emulators (AetherSX2) with blending emulation. FACT: HW path is "serialize per-primitive + GMEM". INFERENCE: Qualcomm's proprietary path likely also needs GMEM-resident attachments, so large shaders + color attachments + fetch could hit a resource/register limit at compile time. No turnip limits for A8xx documented.
12. Vulkan proposal doc for dynamic rendering local read (https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_dynamic_rendering_local_read.html) - FACTS: default mapping is color i <-> InputAttachmentIndex i; input attachment without InputAttachmentIndex means depth/stencil; mapping structs may be chained to pipeline create info when fragment shader state is present; indices must be unique; pipeline mapping must match command-buffer state at draw; with rasterization order extension, the pipeline color-blend/depth-stencil flag bits are usable with dynamic rendering. Relevant for correct variant setup (below).
13. Android Authority (https://www.androidauthority.com/snapdragon-8-elite-8-elite-gen-5-devices-emulation-driver-3617718) - general: emulators have bugs on 8 Elite family; open-source Turnip for A8xx in development (support for 840 expected). No specific failure list. Applicable: an A8xx turnip driver (if available on device) is a useful control experiment; not confirmed available.

Not found / not reachable: Dolphin bug tracker pages (bot-check blocked), Unity issue 9912 (only title: OOM when warming a specific shader on some Adreno), llama.cpp/ncnn/clpeak/Vita3K/RPCS3/Ryujinx/Skyline/Cemu/DXVK/vkd3d-proton/MoltenVK/O3DE/Khronos Vulkan-Samples: searches returned nothing relevant to Adreno -13 on graphics pipelines. Search coverage was limited (a handful of queries), so absence is weak evidence.

## Root-cause candidates (INFERENCE, unverified), ranked
1. Shader size/complexity in the backend (register allocation, ID bound, control-flow with OpSwitch) - matches cases 1, 2, 8.
2. Capability combination (Int8/Int16/Int64/Float16 + 8/16-bit storage + VariablePointers) in a single fragment shader - cases 3, 7.
3. Input attachment / framebuffer-fetch + rasterization-order interplay at link time (needs GMEM fetch lowering) - case 6, 11.
4. Producer-specific SPIR-V shape (structured control flow, OpSwitch with fallthrough/default merge shapes) - cases 4, 5.
5. API-level mismatch (unspecified input-attachment mapping, missing flag bits) - least likely to yield VK_ERROR_UNKNOWN; spec says validity errors are UB but usually surface elsewhere. FACT: shader passes validation and lavapipe builds it.

## Ranked experiments (offline repro matrix)
Priority is by expected information gain, not by API variant order.

Shader-bisect group (highest value, run on baseline API config A):
S1. Replace the fragment shader with a trivial one that keeps the same input attachments, 2 colour outputs, flags and capabilities-declarations (capabilities declared but unused). Passes => API/state is fine; failure is in shader body. Fails => state/capability issue.
S2. Original shader body, but strip unused capabilities (Int8/Int16/Int64/Float16/8-16 bit storage, VariablePointers) via spirv-opt trimming or a rebuild; test each capability removal separately.
S3. Remove rasterization-order flag bits and inout reads together (no fetch) while keeping the body; then re-add only flags. Isolates rast-order.
S4. Binary-search the shader size: truncate/outline functions, replace halves of the body with constants, to find the minimal failing size or the offending construct; count result vs. 14 OpSwitch.
S5. Lower the 14 OpSwitch to if-chains (spirv-opt does not do this directly; do at source or a custom pass) and/or reduce case count.
S6. Run spirv-opt variants: none, -O, -Os, --compact-ids, --eliminate-dead-code-aggressive, --legalize-hlsl-like passes individually (case 5 pattern). Also test Int64 elimination (replace with uvec2 pairs) at source.
S7. Split the shader into several pipelines (per-case/permutation) or specialization constants to shrink; a large module may pass when specialized.
S8. Compare producer: if SPIR-V is non-glslang, round-trip through glslang-style output or spirv-opt, to test case-4 hypothesis.

API-variant group (your A-F, expected value lower unless S1 fails):
A baseline - record exact logcat text ("Failed to link shaders"?).
B explicit VkRenderingInputAttachmentIndexInfo on pipeline create info (with unique indices) - useful because spec default mapping implicit; some drivers only handle explicit.
C explicit VkRenderingAttachmentLocationInfo.
D both B + C.
E mapping derived from the SPIR-V InputAttachmentIndex decorations (must be consistent; also remember command-buffer state must match).
F legacy renderpass with subpasses/self-dependency + same shader (most informative control: if F works, dynamic-rendering-local-read path is the trigger).

Additional variants suggested by cases:
V1. Check/use the pipeline create flag path: for rast-order, use the colour-blend-state flag bit only vs. also the depth/stencil bits; try none (use explicit barriers / VK_DEPENDENCY_BY_REGION instead).
V2. One colour attachment instead of two; input attachment count 1 vs 2 (case 11 GMEM-pressure inference).
V3. Pipeline cache / no-cache / VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED off; creation via vkCreateGraphicsPipelines individually, not batched; try on a fresh VkDevice with only needed features enabled (disable shaderInt64, 8/16-bit storage feature bits at device creation if shader allows).
V4. Pipeline libraries / GPL (if used) vs monolithic pipeline; try monolithic.
V5. Different MSAA/format of attachments (e.g., plain RGBA8 vs formats needing conversion in fetch).
V6. Try a different driver build if available (system Adreno update, or a turnip A8xx build as control) and record driver version; Epic's reports show failures tied to specific builds.
V7. Capture Qualcomm tooling logs: enable verbose driver logging (logcat for adreno/vulkan tags), and the Snapdragon Profiler/"Adreno layer" if available; the compile error string may name the stage.
V8. Prepare a minimal repro (S4 output) and submit via Qualcomm developer support; precedent (cases 8, 10) shows they ask for the shader.

## Facts vs inference summary
- FACT (per fetched pages): cases 1-8, 10-12 symptoms as listed; Qualcomm staff acknowledged compiler issues in 4, 8.
- INFERENCE: everything under Root-cause candidates and "applicable" ratings.
- UNVERIFIED: driver E031.50.19.27 behavior on any public case; any A8xx-specific local_read limit; whether turnip A8xx is usable on the device.

## Memory-derived context
None used.
