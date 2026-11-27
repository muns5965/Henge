# Verification of OSS claims about Adreno VK_ERROR_UNKNOWN (-13)
Method: WebFetch of GitHub pages (gh API was blocked: repos not enabled in session). Page text often lacked open/closed state and dates; flagged below.

1. Cemu #2029 - CONFIRMED (mostly), state unverified
 - Adreno X1-85, Win11 ARM64, driver build f22d572733 (2026-05-22), Cemu graphics pipeline error -13.
 - 105 failed pipelines, each FS had an input (passParameterSem254) with no VS output. Editing 21 SPIR-V cache shaders (drop input, constant vec4(0)) -> 0 failures.
 - Aug 16 2026 update: semantic 254 was not root cause; DEFAULT_VAL from SPI_PS_INPUT_CNTL discarded before Vulkan linkage; generalized fix synthesizes missing VS outputs using DEFAULT_VAL. Tested BOTW + Tekken TT2, no -13. Geometry shader paths unvalidated.
 - Caveat: the "105 -> 0" was from the SPIR-V cache edit, not the source patch.
 - Maintainer Exzap: known limitation; shader key frozen; suggests keeping a fork. No PR link, no merged fix on the page. Open/closed: not shown on page.

2. llama.cpp #28635 - PARTLY / CONTRADICTORY
 - Adreno 830 (SD 8 Elite, RedMagic 10 Pro), proprietary driver Vulkan 1.3.284; vkCreateComputePipelines (COMPUTE, not graphics) -> -13, vkCreateShaderModule OK. 17,320 B fails, 18,844 B works.
 - Title blames shaderc/NDK; comment 2 retracted (17,320 B was truncated extraction / 18,844 B a different base variant; failing _subgroup_no_shmem variant identical across NDK r26/r29 -> driver issue); comment 3 reverses again (same commit, NDK r26=17,320, r29=18,844). Unresolved, no spirv-val/md5 evidence. Failing variant lacks ~104-instruction shared-mem reduction block. No fix/PR. Not "same GLSL proves compiler difference" - treat as unreliable.

3. o3de-extras #625 - PARTLY (reviewer claim mostly right, device detail wrong)
 - Official page: Meta Quest Pro / Quest 3 (Adreno 650 on Quest Pro, Android 12), vkCreateGraphicsPipelines -> -13 for PBR (standardpbr_multiviewpipeline_forwardpass_standardlighting). SkyBox/Grid render, PBR meshes don't.
 - DXC detail confirmed via a mirror (github.kxpsado.workers.dev; not the github.com page): DXC 1.7.2308 fails, 1.6.2112 renders. Cause unconfirmed; Adreno Offline Compiler asserts on both ("Unsupported Instruction in Fast Isel" / "RegIndex == SHARED_CONST_BEGIN"). Workaround: choose DXC via registry (o3de PR #17150, merge unverified). Issue open per mirror. Note: Quest 3 is Adreno 740-class, not 650.
 - Related: Meta forum thread (Quest 2): -13 on vkCreateGraphicsPipelines, blamed on ~40 KB UBO; shrinking it fixed; later Qualcomm driver update fixed it at original size.

4. filament #5294 / PR #6752 / 108cdd4 - CONFIRMED
 - Pixel 4, Android 12, Adreno driver build 11/30/20: SIGSEGV null deref (0x8) in vkCreateGraphicsPipelines, driver libllvm-glnext.so (QGLCCompileToIRShader), with unoptimized shaders (debug). NOTE: a crash, not -13. Suspected cause: functions taking `const mat4` params calling each other (SPIR-V param becomes pointer).
 - Commit 108cdd4 ("vulkan: Fix Adreno crash for debug build (#6752)", ~May 2023, poweifeng): +12/-2 in 3 files. common_defines.glsl adds macro highp_mat4 = `highp mat4` (no const) when TARGET_VULKAN_ENVIRONMENT && TARGET_MOBILE, else `const highp mat4`; used in computeLightSpacePosition (common_shadowing.glsl) and traceScreenSpaceRay (light_reflections.fs). Merged (commit on main). Relevance to us: pattern = Adreno compiler mishandles pointer-typed function params; weak.

5. graphicsfuzz #485 - NOT RELEVANT / contradicted as stated
 - It is "runspv/amber: Vulkan::Calling vkWaitForFences Fail", closed. Says failures are typically driver/OS watchdog timeouts before the 30 s limit; proposes tooling should report TIMEOUT. No device-lost discussion, nothing about -13.

6. gradle #5637 - NOT RELEVANT (Gradle test reports lost when CI interrupts process). Skip.

7. Other Adreno proprietary-driver -13 reports found (all primary pages fetched)
 - phobos PR #292 (merged 2026-10-21): Adreno 740, driver 0676.53, vkCreateComputePipelines, "Shader compilation failed" -> -13 for parallel-rdp rasterizer. Cause: 24 OpSpecConstantOp results stored in shade_pixel (even dead after specialization). Fix: compute those values with ordinary instructions (patched module 164,232 B). Very relevant theme: large compute shader + spec-constant-op stores.
 - aerogauge-recomp PR #61 (merged 2026-09-20): Adreno 750 driver v0762.41, compute pipelines with OR-based 16-bit byteswap -> -13; XOR form works. SPIR-V valid on Turnip/desktop. Theme: pattern-specific compiler bug; rewrite SPIR-V.
 - Pacheco95/sonnet PR #42 (merged 2026-09-26): Adreno 830 (S25 Ultra): OpTypeForwardPointer buffer pointer -> SIGSEGV in libllvm-qgl.so; struct pointer accessed with dynamic index (lights[i].range) -> -13. Workaround: post-pass spirv_for_adreno.py (fold access chains, split struct loads, reorder types).
 - opensro #1239/#1248 (Adreno 735, WebGPU compute -13), alamborghini #367/#370 - same family, not fetched in detail (rate limited).
 - Meta forum: ~40 KB UBO -> -13 on Quest 2 (fixed by driver update).
 - NOT FOUND: any report of -13 tied to input attachments + raster-order access, VK_EXT_rasterization_order_attachment_access, or dynamic rendering local read on Qualcomm proprietary driver; nothing in dxvk/vkd3d-proton/MoltenVK/ANGLE/Godot/Dolphin/Eden hits. PPSSPP driver-bug page has 4 Adreno entries, none about this. Searches were web-search only (could not search GitHub issue search directly), so absence is weak evidence.
 - Also: Lichii #96 (Adreno 740 with Turnip) - Mesa, irrelevant.

## Actionability for our -13 (FS A2Xghfc, 346 KB, 2 input attachments + raster-order access; VS stub vs real)
Actionable:
 - Cemu #2029: test VS stub vs real. If FS inputs lack matching VS outputs, Adreno proprietary driver rejects with -13. Our "VS stub vs real" difference is exactly this class. Check every FS Location input has a VS output with matching type/interpolation; synthesize missing outputs (or remove FS inputs).
 - phobos #292 / aerogauge #61 / sonnet #42: bisect SPIR-V for patterns. Check for OpSpecConstantOp stores, OR-based bit ops, forward pointers, dynamic-indexed struct pointers; rewrite the shader post-pass; use binary reduction (spirv-reduce) on 346 KB to find the trigger.
 - Meta UBO ~40 KB: check uniform/storage buffer sizes in A2Xghfc.
 - Validate SPIR-V (spirv-val) and test same SPIR-V on Turnip/desktop to separate driver bug from invalid SPIR-V; log -13 per pipeline and then null-guard pipeline (aerogauge notes crash on null pipeline bind).
Not actionable / weak:
 - llama.cpp #28635 (contradictory, compute, NDK theory retracted), o3de DXC version (only applies if we compile with DXC; try alternate compiler version cheaply), filament const mat4 (crash not -13), graphicsfuzz #485, gradle #5637, PPSSPP bugs.
 - Nothing public about raster-order/input-attachment -> -13; our test must be experimental (strip the raster-order decoration/feature, one input attachment at a time).
