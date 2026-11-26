# Metal format audit (worktree /tmp/wt-p06, branch research/vertex-fmt-p06)

Sources of values seen: /tmp/x231501/dump/index.txt (cap_att, cap_img, cap_draw, pso lines), /tmp/x230109/dump/index.txt (pso lines, mapped fourcc),
and the two g7_lagfx logs (/tmp/x231501/logs, /tmp/x230109/logs; "arm64 mapped texture ... format N", "g7-gpu-tex-unbound ... mtl_format=N",
"armN-dump pso ... vertex_attrs=[fmtN@off]"). No other g7_lagfx log exists under /tmp (only these two; /tmp/g7lt*, g7jt* are directories of other kinds).
Certainty marks: [dump] = value occurs in a dump; [pub] = public Metal enum value from memory, certain; [unv] = unverified.
Note: "fmt97/103/109" and "color_format=44/97" in the pipeline-build-fail log lines are VkFormat numbers (R16G16B16A16_SFLOAT / R32G32_SFLOAT /
R32G32B32A32_SFLOAT / B8G8R8A8_UNORM), NOT Metal formats.

## Tables checked
- T1 src/vulkan/iosurface.c lagfx_metal_pixel_format_to_vk (x86/IOSurface path; unknown -> warn + BGRA8 fallback)
- T2 src/protocol/arm64_resources.c k_fmt[] (colour) and k_dfmt[] (depth/stencil), the arm64 texture / render-target mapper (unknown -> object refused, TEXWHY_C_FORMAT)
- T3 src/vulkan/pipeline_build.c lagfx_pso_vertex_format_to_vk (vertex; unknown -> VK_FORMAT_UNDEFINED, now a per-attribute error, see P06)
- colour-attachment formats for pipelines are not a Metal table: src/protocol/arm64_exec_run.c copies the render target's VkFormat (from T2/T1) into d.color_formats;
  PSO "colour_formats=not-decoded" in every dump PSO, so no Metal colour format is ever read from a PSO blob.

## A. Pixel formats that occur in the dumps
| MTLPixelFormat | name [pub] | where seen | T1 (iosurface.c) | T2 (arm64 k_fmt/k_dfmt) | status |
|---|---|---|---|---|---|
| 80 | BGRA8Unorm | cap_att 18, cap_img 206, RT/texture logs, mapped format 80 | B8G8R8A8_UNORM | B8G8R8A8_UNORM | mapped; observed = VK 44 in cap_att/cap_draw |
| 115 | RGBA16Float | cap_att 343, cap_img 697, mapped format 115, RT logs | R16G16B16A16_SFLOAT | R16G16B16A16_SFLOAT | mapped; observed = VK 97 |
| 30 | RG8Unorm | g7-gpu-tex-unbound mtl_format=30 (231501 x6, 230109 x2), linear_texture, why=not_mirrored | UNMAPPED (-> warn + BGRA8) | R8G8_UNORM | mapped in the arm64 mapper (unobserved on device: those textures were unbound), unmapped in iosurface.c; iosurface.c is not on the arm64 path |
| 1 | A8Unorm | g7-gpu-tex-unbound mtl_format=1 (230109 x5; none in 231501) | R8_UNORM | R8_UNORM (+ view swizzle to alpha) | mapped |
| 0 | Invalid | one "mtl_format=0" in 231501 (g7-gpu-tex-unbound line, absent RT slot) | unmapped | unmapped | not a format (absent RT), nothing to map |
| fourcc 0x42475241 ("ARGB"), 0x52476841 ("AHGR") | mapped-surface fourcc, not MTLPixelFormat | mapped lines (231501: 43 ARGB, 4 AHGR; 230109: 50 ARGB, 3 AHGR) | n/a | RGHA -> R16G16B16A16_SFLOAT, else B8G8R8A8_UNORM (81 -> SRGB) | mapped |
Depth: every cap_pass has depth=0 / depth_vkfmt=0 in x231501; no depth or stencil Metal format occurs in either dump.
So no dump value is unmapped in the table that actually serves it. The only unmapped-but-observed value is 30 in T1, which no dump path calls.

## B. Vertex formats that occur in the dumps
| MTLVertexFormat | name [pub] | where seen | T3 | status |
|---|---|---|---|---|
| 29 | Float2 | 46 / 44 pso vertex_attrs, vtx_attrs=29:16,29:24 | R32G32_SFLOAT | mapped [dump] |
| 27 | Half4 | 23 / 22 pso vertex_attrs, vtx_attrs=27:32 | R16G16B16A16_SFLOAT | mapped [dump] |
| 31 | Float4 | not in any PSO blob; x86 draw_emit.c prepends it as the implicit attribute 0 | R32G32B32A32_SFLOAT | mapped (ground truth per code comment, lldb); not in dump files |
No unknown vertex format occurs in either dump or log (grep "unknown Metal pixel format|unknown...vertex format": 0 hits in all four logs).
9 (UChar4Normalized) is documented in code as observed on device in an earlier run, not in these two dumps.

## C. Full public vertex-format enum vs T3 (for the hardening change; certain from the public MTLVertexFormat header)
Mapped (T3): 3 UChar4 -> R8G8B8A8_UINT [pub; semantics: Metal UChar4 is an *integer* attribute, accepted]; 9 UChar4Normalized -> B8G8R8A8_UNORM
[pub value; the BGRA choice is a device finding, not derivable from the header]; 27 Half4; 28 Float; 29 Float2; 30 Float3; 31 Float4;
37 UInt2 -> R32G32_UINT [verified earlier]; 52 ShortNormalized -> R16_SNORM [verified earlier].
Verified-by-user earlier but deliberately unmapped: 36 UInt, 42 UChar4Normalized_BGRA (test pins 36 as UNDEFINED).
Unmapped (all [pub] values, none observed, none added): 1 UChar2, 2 UChar3, 4-6 Char2-4, 7 UChar2Normalized, 8 UChar3Normalized, 10-12 Char*Normalized,
13-15 UShort2-4, 16-18 Short2-4, 19-21 UShort*Normalized, 22-24 Short*Normalized, 25 Half2, 26 Half3, 32-35 Int..Int4, 38 UInt3, 39 UInt4,
40 Int1010102Normalized, 41 UInt1010102Normalized, 42 UChar4Normalized_BGRA, 45-48 UChar/Char/UCharNormalized/CharNormalized, 49 UShort, 50 Short,
51 UShortNormalized, 53 Half, plus 0 Invalid. With the P06 change any of these in a decoded PSO now fails that PSO's build (LAGFX_ERR_PROTOCOL, one log line)
instead of silently repacking everything as float; supporting them is a separate, evidence-driven decision. Not added: nothing in the dumps justifies it, and
several have device-optional Vulkan vertex support (e.g. R8G8B8_*, R16G16B16_*) or non-trivial 1010102 packing.

## D. Pixel-format table review against the public MTLPixelFormat enum (all [pub], certain)
T2 colour table entries all agree with the public enum numbers: 1 A8Unorm, 10-14 R8 (Unorm, sRGB, Snorm, Uint, Sint), 20 R16Unorm, 22-24 R16 Snorm/Uint/Sint, 25 R16Float,
30-34 RG8, 53-55 R32 Uint/Sint/Float, 60/62-65 RG16, 70-74 RGBA8, 80/81 BGRA8, 90 RGB10A2Unorm, 92 RG11B10Float, 93 RGB9E5Float, 103-105 RG32, 110/112-115 RGBA16,
123-125 RGBA32. Depth/stencil: 250 Depth16Unorm, 252 Depth32Float, 253 Stencil8, 255 Depth24Unorm_Stencil8, 260 Depth32Float_Stencil8.
Mapped but unverified on a device (no dump uses them): everything in T2 other than 1, 80, 115 (and 25 / 250, verified earlier). 
T1 (iosurface.c) knows only 80, 70, 115, 1/10, 252, 25, 250. It is NOT used by the arm64 mapper (arm64_resources.c says so). The upstream (MIT) copy had 25 -> D16, which
this repo already corrected. Gaps in T1 relative to T2 (e.g. 30 RG8Unorm, 55, 65, 81, 71): not added, because no dump or log path reaches T1 with those values.
