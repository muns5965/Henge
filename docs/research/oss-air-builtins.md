# AIR vertex/fragment/compute built-ins and attributes: licence-clean evidence (X13, X14)

Read-only research. No code was changed. All paths are absolute; "line" numbers are 1-based lines of the file at the pinned commit.
Working data kept: /tmp/oss_work (disassembled .ll of the 40+40 modules, scripts in /tmp/oss_work/py). Shallow clones in /tmp/oss_clone were deleted at the end.

Legend: READ = literally in the cited source or dump. INFER = my deduction from READ facts. Confidence H/M/L.

----------------------------------------------------------------------
## 1. What the dumps actually contain (anchor data)

Method: the 40 `lib_*.bin` (MTLB) modules per dump were already extracted as bitcode to /tmp/census_work/bc1 (= x231501) and bc2 (= x230109) (bc1 hashes: 40/40 in x231501, 38/40 in x230109; bc2: 38/40 and 40/40). `llvm-dis` IS present on this box (/usr/bin/llvm-dis, LLVM 18.1.3) and disassembles these modules (compiler string "Apple metal version 32023.622 (metalfe-32023.622)", triple air64-apple-macosx15.6.0, `!air.version = !{i32 2, i32 7, i32 0}`). All 80 disassembled to /tmp/oss_work/ll1 and ll2. Counts below come from the `.ll` (named-metadata nodes) and were cross-checked with a raw-byte scan of every `lib_*.bin`, plus the 113 (x231501) / 43 (x230109) `pso_*`/`cap_*` blobs (those contain zero AIR metadata strings).

Both dumps give identical numbers (same 40-module shader set, 29 fragment, 10 vertex, 1 kernel entry points per dump).

### 1.1 Attribute/metadata strings that OCCUR (per dump; "mods" = modules containing it, "nodes" = metadata node lines)

| string | mods | nodes / note |
|---|---|---|
| `!air.fragment` / `!air.vertex` / `!air.kernel` | 29 / 10 / 1 | named metadata, one entry node per entry point |
| `air.position` | 39 | 45 fragment-input nodes (`{i32 N, "air.position", "air.center", "air.no_perspective", TYPE, NAME [, "air.arg_unused"]}`) + 10 vertex-output nodes (`{"air.position", "air.arg_type_name", "float4", "air.arg_name", N}`, no index operand, no interpolation) |
| `air.vertex_id` | 10 (all vertex modules) | 10 nodes: `{i32 N, "air.vertex_id", "air.arg_type_name", "uint" [, "air.arg_name", "vid"]}`; IR param is `i32` (e.g. VfxXgh: `i32 noundef %1`; ll1/445d43e9ba751f49_VfxXgh.ll) |
| `air.vertex_output` | 10 | 18 nodes: `{"air.vertex_output", "generated(<len><name><Itanium type>)", "air.arg_type_name", T, "air.arg_name", N}` |
| `air.fragment_input` | 29 | 48 nodes, ALL `"air.center", "air.perspective"` (zero flat/centroid/sample/no_perspective on user varyings) |
| `air.center` | 29 | always paired with `air.perspective` (user varyings) or `air.no_perspective` (position) |
| `air.perspective` / `air.no_perspective` | 29 / 29 | no_perspective only on `[[position]]` inputs |
| `air.render_target` | 30 | 29 outputs `{"air.render_target", i32 RT, i32 0, ["air.arg_type_name", T, "air.arg_name", N]}` (28x `(0,0)`, 1x `(1,0)`), + 7 framebuffer-fetch INPUT nodes `{i32 N, "air.render_target", i32 RT, TYPE, NAME}`; plus 1 imageblock member `{..."air.render_target", i32 0}` |
| `air.point_size` | 1 | VfxXgh vertex output `{"air.point_size", "air.arg_type_name", "float", "air.arg_name", "point_size"}`; return struct member is plain `float` |
| `air.point_coord` | 1 | A2Xghfc fragment input `{i32 7, "air.point_coord", "air.arg_type_name", "float2", "air.arg_name", "point_coord"}`; IR `<2 x float>` |
| `air.threadgroup_position_in_grid` | 1 | kernel tile_simd_blur_15, `ushort2`, IR `<2 x i16>` |
| `air.threads_per_threadgroup` | 1 | same kernel, `ushort2`, `<2 x i16>` |
| `air.thread_index_in_simdgroup` | 1 | same kernel, `ushort` (`simd_lane`), `i16` |
| `air.simdgroup_index_in_threadgroup` | 1 | same kernel, `ushort` (`simd_group`), `i16` |
| `air.imageblock` | 1 | kernel arg 0 `{i32 0, "air.imageblock", "implicit", "air.struct_type_info", !N, ...}`; struct member `{i32 0, i32 8, i32 0, "half4", "color", "air.render_target", i32 0}` |
| `air.raster_order_group` | 1 | on an `air.texture` arg (`inplace_copy_lph`): `..., "air.write", "air.raster_order_group", i32 0, ...` |
| `air.sample` | 24 | ALL 37 hits are TEXTURE ACCESS (`"air.texture", "air.location_index", i32 N, i32 1, "air.sample"`), NOT interpolation and NOT sample_id |
| `air.struct_type_info`, `air.arg_type_name`, `air.arg_name`, `air.arg_unused`, `air.location_index`, `air.buffer`, `air.buffer_size`, `air.address_space`, `air.arg_type_size`, `air.arg_type_align_size`, `air.read`, `air.write`, `air.read_write`, `air.texture`, `air.sampler`, `air.sampler_state`, `air.workaround.sampler_state_variable_length_encoding` | many | generic arg descriptors (not in X13 scope) |
| `air.compile.denorms_disable`, `air.compile.fast_math_enable`, `air.compile.framebuffer_fetch_enable`, `air.max_*` module flags | 40 | module-level |

### 1.2 Names with ZERO occurrences in either dump (libs, pso blobs, cap blobs)
`air.instance_id`, `air.base_vertex`, `air.base_instance`, `air.front_facing`, `air.sample_id`, `air.sample_mask`, `air.sample_mask_in`, `air.render_target_array_index`, `air.viewport_array_index`, `air.amplification_id`, `air.invariant`, `air.depth`, `air.depth_qualifier`, `air.stencil`, `air.flat`, `air.centroid`, `air.stage_in`, `air.vertex_input`, `air.clip_distance`, `air.primitive_id`, `air.thread_position_in_grid`, `air.thread_position_in_threadgroup`, `air.thread_index_in_threadgroup`, `air.threads_per_simdgroup`, `air.barycentric_coord`, `air.position_in_patch`, `air.patch_id`, `air.early_fragment_tests`, `air.interpolation_function`, `air.max_work_group_size`, `air.function_constants`.
This confirms and refines census X13 ("0 of 40 modules carry any of the unhandled keys"; "air.no_perspective appears 29x only on [[position]]" is exactly right, 45 nodes in 29 modules).

### 1.3 Other READ facts from the dumps that matter for the translator
* Stage-in is FLATTENED. There is no `air.stage_in` record anywhere. A fragment `[[stage_in]]` struct becomes one `air.fragment_input` node per member; a vertex return struct becomes one `air.position`/`air.vertex_output`/... node per member in `!air.vertex` operand 1 (the "outputs" list). Fragment: operand 1 = list of outputs (`air.render_target` ...), operand 2 = list of inputs (each with leading `i32` = LLVM parameter index).
* Varying identity: `air.vertex_output` and `air.fragment_input` carry the same `generated(<len><name><mangled type>)` string; e.g. `generated(5colorDv4_Dh)` = name `color`, type `half4` (`Dv4_Dh` = 4-vector of half; `Dv2_f` = float2; `Dv4_f` = float4). The 11 distinct strings on the vertex side are exactly the 11 on the fragment side (READ). So varyings are linked by this string, not by a location number. (`air.location_index` on varyings never occurs.)
* Return type of a vertex/fragment function is a packed literal struct whose members are in the same order as the outputs list (`<{ <4 x float>, float, <2 x float> ... }>`), `air.position` first.
* `air.render_target` output = `(render_target_index, second_int)`; second int is 0 in all 29 nodes. INFER (H): second int is MSL `index(i)` (dual-source); see section 3.
* Subgroup size in the one kernel is obtained by a CALL `@air.get_simdgroup_size.i16()`, not by an `air.threads_per_simdgroup` argument (so SPIR-V SubgroupSize has two possible AIR encodings; only the intrinsic is dump-evidenced).
* Compute built-in argument types follow the MSL declaration (`ushort2` -> `<2 x i16>`, `ushort` -> `i16`): the AIR attribute does not fix the width, the declared type does.

----------------------------------------------------------------------
## 2. Sources used, with licence status

| source | commit | licence (how checked) | status |
|---|---|---|---|
| vertex-language/air | 3d277d16858a7141eb021fd614f021b595c13978 | MIT (LICENSE read) | used (kernels only; no vertex/fragment) |
| KhronosGroup/SPIRV-Cross (MSL backend) | f0edde12d69ea7a498da14af05ec14f390b07c98 | Apache-2.0 (LICENSE; spirv_msl.cpp SPDX "Apache-2.0 OR MIT") | used |
| KhronosGroup/SPIRV-Headers (grammar json) | f6f78c717057ac7f5f3ea021f3f962aaf9d4da7e | Khronos MIT (LICENSE read) | used for capabilities |
| LuisaGroup/LuisaCompute | aa169200d19d29f691a6f34da916ff61d2931d59 | Apache-2.0 (LICENSE read; no per-file header) | used; emits real AIR for Apple GPUs |
| gzorin/LLAIR | 0a9246bf8cfca1790afd337c60abcda4d200b26d | University of Illinois/NCSA (permissive BSD-style; LICENSE.TXT read) | used (2017, older AIR dialect) |
| CrossGL/crosstl | d698cde012f05b2329600c69dbcac6634b78bc09 | Apache-2.0 (LICENSE read) | used (test-harness table of AIR names) |
| zhuowei/MetalShaderTools | 820e4301bc85c472bf5c44ebf2c8660fec0d0f70 | MIT (LICENSE read) | used (real xcrun output + MSL source, 2018 compiler) |
| DsoTsin/kaleido3d | 96e5c83327e05e204f938642745a1862d578b69d | MIT (LICENSE read) | used with LOW weight (see note) |
| xenios-jp/spirv2air | 58375aecf2ca894055a6fb452d58b8d4cc45cc39 | BSD-3-Clause (LICENSE read: 3 clauses incl. no-endorsement; repo-level only) | used with MEDIUM weight (2026 single-project, provenance of its metadata knowledge not stated) |
| Apple "Metal Shading Language Specification" PDF (public) | fetched 2026-11-27 via WebFetch | public spec | used for semantics; text came through a summariser, so table cells are paraphrase-grade READ |
| Mesa, MoltenVK | not cloned | Mesa MIT / MoltenVK Apache-2.0 | Mesa has no AIR consumer; MoltenVK just drives SPIRV-Cross (same attribute table) |

Skipped as forbidden or licence-unclear (not read; only seen as names in a code-search hit list):
* 3Shain/dxmt (and its forks: mullion, EmuPC, VirtualBox copy, pear-dxmt, etc.): LGPL-2.1+ (LICENSE/COPYING.LIB read). FORBIDDEN class.
* steelbrain/* (metal2vulkan, reims-vgpu), anyos/medfa12, Leviidev/Henge, /tmp/ref/r: forbidden by instruction, not opened (search hits filtered out).
* microlift/rosemary, SamoZ256/luft: GPL-3.0 (LICENSE read). FORBIDDEN.
* nullmoth/nvidia-macos-driver (and its many copies): PolyForm Noncommercial 1.0.0 (not in the allowed list). Skipped.
* PureDarwin/PureDarwin: no top-level LICENSE in the checkout; mixed Apple-derived content. Licence unclear: skipped.
* oosexgang/hakios-xnu-kernel and its dozen clones (`air_to_nir.c`, `*.ll` fixtures): XNU-derived tree with a LICENSES directory (APSL-class); licence unclear: skipped.
* UniXNU/UniXNU (+ marble, sheng): top-level BSD-2-Clause-Patent but XNU-derived and bundles metal2vulkan (forbidden) material in sibling repos; licence unclear for the AIR scripts: skipped.
* nfzerox/VirtualIntelMacOnAppleSilicon: MIT at top level but project is an unrelated guest-driver shim whose provenance I could not check: not used.
* Other code-search hits not cloned (licence not checked): jdrly/d9mt, v0lumehi/nxkit, Nik300/viva-gpu, MarcoDotIO/Ycode, Snapintosh/air2ir3, MC-XiaoXiao/ios_sagit, garvitsurana271/stambha, brunostjohn/ios-on-switch-lmao, apple-cross-toolchain/rules_applecross: licence unclear: skipped.

Note on kaleido3d (2015-era, MIT): its `air_builtin_to_spirv` has entries that look speculative (`air.patch_id` -> PatchVertices, `air.sample_mask_in || air.sample_mask` both -> SampleMask). Treat it as corroboration only, never as sole evidence.

----------------------------------------------------------------------
## 3. Findings (per finding: repo@commit, file:line, licence, paraphrase, READ/INFER, confidence)

### F1 Compute built-in descriptor names and types (X14)
* vertex-language/air@3d277d1, /kernel.go:51-87 (enum + name table; names at 74-86), MIT. Paraphrase: kernel argument descriptor names are `air.thread_position_in_grid`, `air.thread_position_in_threadgroup`, `air.threadgroup_position_in_grid`, `air.threads_per_threadgroup`, `air.threadgroups_per_grid`, `air.threads_per_grid`, `air.thread_index_in_threadgroup`, `air.thread_index_in_simdgroup`, `air.simdgroup_index_in_threadgroup`, `air.simdgroups_per_threadgroup`, `air.threads_per_simdgroup`, `air.dispatch_threads_per_threadgroup`. READ. H (this repo is oracle-tested against xcrun; oracle_test.go exists).
* same, /kernel.go:99-113: allowed declared types are `uint`/`ushort`, plus 2/3-vectors for the grid positions only (thread_position_*, threadgroup_position_in_grid, threads_per_threadgroup, threadgroups_per_grid, threads_per_grid, dispatch_threads_per_threadgroup); the index/count ones are scalar. READ. H.
* same, /metadata.go:127-135 (argNode): a built-in argument node is `{i32 argIndex, "air.<builtin>", "air.arg_type_name", T, "air.arg_name", N [, "air.arg_unused"]}`, exactly the shape in the dump (tile_simd_blur_15). READ. H (dump-confirmed).
* same, /metadata.go:92-104: kernel entry node = `{fn, {} (empty outputs), {args...} [, {"air.max_work_group_size", n}]}`; dump confirms `{fn, !{}, !{...}}`. READ. H.
* same, /kernel.go:63-67: DispatchThreadsPerThreadgroup differs from ThreadsPerThreadgroup in the last, non-dividing group of a dispatchThreads grid. READ. H. (Relevant: SPIR-V WorkgroupSize is the dispatched size, so `air.threads_per_threadgroup` may be smaller than WorkgroupSize in the tail group; map WorkgroupSize to `air.dispatch_threads_per_threadgroup` when exact semantics are needed. INFER, M.)
* LuisaCompute@aa16920, /src/backends/metal4/llvm_codegen/metal_codegen_llvm_metadata.cpp:499-518, Apache-2.0: a production kernel declares `thread_position_in_threadgroup`, `threadgroup_position_in_grid`, `thread_position_in_grid`, `threads_per_threadgroup` as `uint3`, and `threads_per_simdgroup`, `thread_index_in_simdgroup` as `uint`. READ. H.
* SPIRV-Cross@f0edde1, /spirv_msl.cpp:18807-18823 (builtin_qualifier) and 18998-19008 (builtin_type_decl), Apache-2.0: GlobalInvocationId->thread_position_in_grid, WorkgroupSize->threads_per_threadgroup, WorkgroupId->threadgroup_position_in_grid, NumWorkgroups->threadgroups_per_grid, LocalInvocationId->thread_position_in_threadgroup, LocalInvocationIndex->thread_index_in_threadgroup (all uint3 except Index = uint). READ. H.
* SPIRV-Cross, /spirv_msl.cpp:18825-18879: SubgroupSize->threads_per_simdgroup (thread_execution_width before MSL 3.0; kernel/compute), SubgroupLocalInvocationId->thread_index_in_simdgroup, SubgroupId->simdgroup_index_in_threadgroup, NumSubgroups->simdgroups_per_threadgroup (quadgroup variants if quad ops are requested). READ. H.
* Dump READ (section 1): only 4 of these appear (threadgroup_position_in_grid, threads_per_threadgroup, thread_index_in_simdgroup, simdgroup_index_in_threadgroup), all as ushort/ushort2; SubgroupSize is an intrinsic call there.
* MSL spec Table 5.8 (Sec. 5.2.3.6; WebFetch of developer.apple.com/metal/Metal-Shading-Language-Specification.pdf): thread_position_in_grid etc. accept ushort/ushort2/ushort3/uint/uint2/uint3; thread_index_in_simdgroup, simdgroup_index_in_threadgroup, threads_per_simdgroup accept ushort or uint, macOS Metal 2+; thread_execution_width is a deprecated alias. READ (summariser). M-H.

### F2 vertex_id, instance_id, base_vertex, base_instance
* Dump: `air.vertex_id` READ (10 nodes, IR i32, MSL type "uint"). Others not in dump.
* Metadata form of the others: spirv2air@58375aec, /src/spirv2air/translate.cpp:876-879, BSD-3: `{"air.vertex_id","air.arg_type_name","uint","air.arg_name",...}` then `{"air.base_vertex","air.arg_type_name","uint",...}`, both i32 parameters; and :880 comment "vertex_id - base_vertex". READ. M.
* LuisaCompute, /metal_codegen_llvm_metadata.cpp:598-606, Apache-2.0: vertex-function args `air.vertex_id`, `air.instance_id`, `air.base_instance`, each `uint`. READ. H (runs on real Metal).
* crosstl@d698cde, /tools/coverage/decorations.py:100-101,119-120, Apache-2.0: VertexIndex->air.vertex_id, InstanceIndex->air.instance_id, BaseVertex->air.base_vertex, BaseInstance->air.base_instance. READ. M.
* SPIRV-Cross, /spirv_msl.cpp:18691-18702 (names), 14250-14258 (all six SPIR-V ids are valid vertex inputs and become `[[...]]`), 18485-18590: GL-style 0-based VertexId/InstanceId is produced by `gl_VertexID - gl_BaseVertex` (so Metal's vertex_id includes base vertex); VertexIndex/InstanceIndex are used as-is; BaseVertex/BaseInstance require Metal 1.1 and macOS or Apple A9+. READ. H.
* MSL spec Table 5.2 (Sec. 5.2.3.1): "vertex_id: The per-vertex identifier, which includes the base vertex value if one is specified"; instance_id likewise includes base instance; types ushort/uint. READ (summariser). H.
* INFER (H): SPIR-V `VertexIndex`/`InstanceIndex` (Vulkan semantics, include firstVertex/firstInstance) map 1:1 to `air.vertex_id`/`air.instance_id`; `BaseVertex`/`BaseInstance` map to `air.base_vertex`/`air.base_instance`. SPIR-V `VertexId`/`InstanceId` are not valid in Vulkan; if ever seen, emit vertex_id - base_vertex like SPIRV-Cross.

### F3 position, invariant, point_size, point_coord, clip distance
* Dump READ: vertex `air.position` has NO interpolation operands; fragment `air.position` is `"air.center","air.no_perspective"` (45 nodes). point_size and point_coord shapes in section 1.1.
* LLAIR@0a9246b, /lib/IR/EntryPoint.cpp:761-795 (position metadata writer) and 507-577 (reader), NCSA; LuisaCompute /metadata.cpp:547-551, 694-698; spirv2air /translate.cpp:942-944, 1039-1042: same shapes. READ. H.
* spirv2air /translate.cpp:1039-1041: invariant position = `{"air.position","air.invariant","air.arg_type_name","float4","air.arg_name","position"}` (token directly after air.position). READ. M-L (no second source for the token position; crosstl decorations.py:114 only gives the name `air.invariant`). MSL spec Table 5.4: invariant needs MSL 2.1 and `-fpreserve-invariance`. READ (summariser).
* SPIRV-Cross /spirv_msl.cpp:18710-18718 (`position, invariant`; MSL 2.1+), 14285-14290 + spirv_msl.hpp:331 (`enable_point_size_builtin`: point_size attribute is only emitted when really rendering points, because Metal rejects it on non-point pipelines). READ. H. Relevant to emitting `air.point_size`: a pipeline-state dependency.
* spirv2air :1043-1054; crosstl metal_reflection.py:224-228 (Apache-2.0): clip distances are ONE `air.clip_distance` output with `air.clip_distance_array_size, N` (max 8, no companion varyings); crosstl notes that a MSL `float x [[clip_distance]]` single becomes size 1. READ. M. Cull distance has no AIR attribute (spirv2air :1045-1046; SPIRV-Cross emits `[[user(cullN)]]` /spirv_msl.cpp:14311). READ. M.
* point_coord: SPIRV-Cross :18777 (float2). MSL Table 5.5: float2 in [0,1]. READ.

### F4 Fragment inputs: front_facing, sample_id, sample_mask (in), position, barycentrics, layer/viewport
* spirv2air /translate.cpp:945-947: `{"air.front_facing","air.arg_type_name","bool","air.arg_name","front_facing"}`, IR param `i1`. READ. M. LuisaCompute /metadata.cpp:762-764 (same, `bool`). READ. H. crosstl decorations.py:98. READ.
* spirv2air /translate.cpp:1086-1096: `air.sample_id` (uint, IR i32) and `air.sample_mask_in` (uint, i32; note the `_in` suffix for the INPUT form). Comment at :1081: taking the sample index "makes a fragment function run once per sample". crosstl decorations.py:139-141 states the same `air.sample_mask_in` naming (`AIR_INPUTS`). kaleido3d spirv_util.cc:136-139 accepts both `air.sample_mask_in` and `air.sample_mask`. READ x3 (not independent). M. (No Apple-compiler output sample found; dump has none.)
* SPIRV-Cross /spirv_msl.cpp:18775-18783: FrontFacing->front_facing, PointCoord->point_coord, FragCoord->position, SampleId->sample_id, SampleMask->sample_mask (input and output share the `[[sample_mask]]` spelling in MSL); types bool, float2, float4, uint, uint (18967-18976). READ. H. SPIRV-Cross also rewrites SampleMaskIn when sample-rate is needed (18622-18630; 1916-1919 `force_sample_rate_shading`).
* Barycentrics (not in task list, noted): `air.barycentric_coord` + `air.center` + `air.no_perspective|perspective` + float3 (spirv2air :954-955; LuisaCompute :758-761 uses `air.center, air.perspective`). SPIRV-Cross :14437-14438, 18872-18878 (MSL 2.2 macOS / 2.3 iOS). READ. M.
* render_target_array_index / viewport_array_index as FRAGMENT inputs: SPIRV-Cross /spirv_msl.cpp:14436 + 18721-18726; crosstl decorations.py:107-108; kaleido3d spirv_util.cc:126-129. MSL Table 5.5 allows both as fragment inputs (uchar/ushort/uint for RT index in vertex output; uint in fragment input). No AIR-metadata example with full operand shape found in the allowed set beyond the bare name; assume the generic built-in shape `{[i32 N,] "air.render_target_array_index", "air.arg_type_name", "uint", "air.arg_name", N}`. INFER. M-L.

### F5 Vertex outputs: render_target_array_index, viewport_array_index, amplification_id
* crosstl decorations.py:107-108,132 (Apache-2.0): Layer->air.render_target_array_index, ViewportIndex->air.viewport_array_index, ViewIndex->air.amplification_id. READ. M (test-harness anchors; presumably observed in real compiler output but not shown).
* SPIRV-Cross /spirv_msl.cpp:14292-14301 (vertex output: `[[viewport_array_index]]`, `[[render_target_array_index]]`, type `uint` at 18937-18941; ViewportIndex requires Metal 2.0), :14436, :18784-18792 (in a fragment shader ViewIndex is read back from `render_target_array_index`; in vertex shaders ViewIndex is derived from the instance index, i.e. SPIRV-Cross does NOT use amplification_id). READ. H.
* MSL spec Tables 5.4/5.2 (summariser): render_target_array_index macOS Metal 1.1+, viewport_array_index macOS Metal 2+, `amplification_id` Metal 2.3+ macOS (the "array index offset mapping" for vertex amplification, range [0, amplification_count)). READ. M-H.
* INFER (L-M): `ViewIndex` is a plausible but unproven analogue of `air.amplification_id`; their semantics differ (amplification_id is an offset into viewport/RT-array mapping tables set by the API; SPIR-V multiview ViewIndex indexes views). Nothing in the allowed sources shows the full metadata shape of `air.amplification_id` (only crosstl's name). UNKNOWN: operand layout, `amplification_count` argument, return-struct handling.

### F6 Fragment outputs: depth, stencil, sample_mask
* Depth: spirv2air /translate.cpp:293 (default `"air.any"`), 509-514 (DepthLess -> `"air.less"`, DepthGreater -> `"air.greater"`), 1057-1059: output node `{"air.depth","air.depth_qualifier","air.any|air.greater|air.less","air.arg_type_name","float","air.arg_name","depth"}`. LuisaCompute /metadata.cpp:652-668: identical shape, qualifiers `air.any`/`air.greater`/`air.less` (note LuisaCompute names the enums GREATER_EQUAL / LESS_EQUAL). LLAIR /EntryPoint.cpp:1368-1403 reads the same three qualifier strings after the key `air.depth_qualifier`. READ x3. H. (crosstl decorations.py:99,117 corroborates `air.depth`, `air.greater`.)
* Stencil: spirv2air :1061-1064: `{"air.stencil","air.arg_type_name","uint","air.arg_name","stencil"}`, IR i32; only emitted when the shader stores it. crosstl decorations.py:133 (name only). SPIRV-Cross :18803-18804 + 14522-14532 (MSL 2.1+; only emitted if `enable_frag_stencil_ref_builtin` because Metal rejects it for pipelines without a stencil attachment). MSL Table 5.7: stencil is uint, Metal 2.1+. READ. M-H.
* Sample mask output: spirv2air :1066-1068: `{"air.sample_mask","air.arg_type_name","uint","air.arg_name","sample_mask"}`, IR i32 (output form has NO `_in`). crosstl decorations.py:106. SPIRV-Cross :14539-14540 (`[[sample_mask]]`). READ. M.
* Color outputs: `{"air.render_target", i32 RT, i32 SECOND, ["air.arg_type_name", T, "air.arg_name", N]}` (dump: (0,0)x28, (1,0)x1; LuisaCompute :647-650 `(i, 0)`; spirv2air :1016 `(location, 0)`; LLAIR :1405-1431 reads two ints "location0/location1"). READ. H for the shape. INFER (H): SECOND = MSL `index(i)` (SPIRV-Cross :14551 `[[color(locn), index(N)]]` for dual-source; MSL Table 5.7 "index(i) default 0"). No dump or source shows SECOND != 0 in AIR, so the dual-source encoding is UNPROVEN.
* Output ordering in return struct: spirv2air :995-1001 comment: varyings/colour targets in Location order, then position/depth/coverage. LuisaCompute emits render_targets first, then depth. READ. M (order within the list likely free; return struct member order must match the list).

### F7 Interpolation qualifiers (fragment_input and, in older AIR, vertex_output)
* Encoding (two strings, or one for flat), READ in three allowed repos:
  * LLAIR /EntryPoint.cpp:520-560 (reader) and 761-790, 816-848 (writer): `air.center|air.centroid|air.sample` followed by `air.perspective|air.no_perspective`, or the single token `air.flat`; default when absent = center+perspective.
  * LuisaCompute /metadata.cpp:707-738: all seven combinations, `air.flat` single token, placed right after the `user(...)` identity string and before `air.arg_type_name`.
  * spirv2air /translate.cpp:930-938: same; flat replaces the pair. Pull-model `interpolant<T,...>` inputs use `"air.interpolation_function"` + perspective token (924-925).
  READ. H.
* Dump READ: only `center`+`perspective` (48 nodes) and, on `[[position]]`, `center`+`no_perspective` (45 nodes). H for those two.
* Table (AIR tokens <-> MSL <-> SPIR-V decoration), see section 4.
* SPIRV-Cross /spirv_msl.cpp:14455-14512 (Apache-2.0): MSL attribute chosen by decorations: Flat -> `flat` (integers are not annotated: "the default (in fact, the only option)"), Centroid(+NoPerspective) -> centroid_perspective/no_perspective, Sample(+NoPerspective) -> sample_*, NoPerspective alone -> center_no_perspective; FragCoord never annotated ("always noperspective on Metal"). READ. H.
* MSL spec Sec. 5.4 (summariser): default center_perspective; `[[position]]` only center_no_perspective; integer variables only flat; sample_* interpolation makes fragment code that uses the variable execute per sample. READ. M-H. => INFER (H): any integer varying must be `air.flat`; a `Sample` decoration or SampleId read forces sample-rate shading (needs SPIR-V capability SampleRateShading).
* Vertex output side: in the 2026 dumps `air.vertex_output` carries NO interpolation tokens (READ, 18 nodes); LLAIR (2017 compiler) wrote `air.center/air.perspective` after arg_name on vertex outputs (/EntryPoint.cpp:1151-1170). Interpretation: interpolation is decided on the fragment_input side in current compilers; ignore vertex-side tokens. INFER. M.

### F8 [[stage_in]] struct and vertex-fetch attribute metadata
* MetalShaderTools@820e430, /sampleshader/AAPLShaders.metal:16-29,31-34,68 and /sampleshader/out_vertexShader.air.ll:47-54, /out_fragmentShader.air.ll:28-31, MIT: MSL `struct RasterizerData { float4 clipSpacePosition [[position]]; float4 color; }`, vertex returns it, fragment takes `RasterizerData in [[stage_in]]`. Real compiler output: vertex outputs list = `{"air.position", ..., "clipSpacePosition"}`, `{"air.vertex_output","generated(5colorDv4_f)", ..., "color"}`; fragment args = `{i32 1, "air.fragment_input","generated(5colorDv4_f)","air.center","air.perspective","air.arg_type_name","float4","air.arg_name","color"}`; render target `{"air.render_target", i32 0, i32 0, "air.arg_type_name","float4"}`. No `air.stage_in`, no struct-level node. READ. H (matches the 2026 dumps exactly, 8 years earlier compiler).
* `generated(<len><member name><Itanium-mangled type>)` is the identity for members without `[[user(...)]]`; with `[[user(name)]]` the identity is `user(name)` (spirv2air :917-926, 1011; LuisaCompute :553 `user(locnN)`, :700). READ. M-H.
* Vertex `[[stage_in]]` (vertex attributes fed by MTLVertexDescriptor): `{i32 N, "air.vertex_input", "air.location_index", i32 ATTR, i32 1, "air.arg_type_name", T, "air.arg_name", NAME}`. LuisaCompute /metadata.cpp:581-588 (Apache-2.0); LLAIR /EntryPoint.cpp:465-506 reads `air.vertex_input` + `air.location_index` with two ints. READ. M-H. NOT in the dumps: 9 of the 10 vertex modules are `air.vertexFetchFunction` (driver-generated fetch function whose inputs are `air.buffer` args plus `air.vertex_id`, outputs `air.position` + `air.vertex_output`), and the 10th (VfxXgh) also reads vertex data from an `air.buffer` arg.
* `air.stage_in`: only appears in crosstl metal_reflection.py:62 (a list of argument kinds) and kaleido3d spirv_writer.cc:315 (handler). Neither shows a shape; Apple's compiler does not emit it for ordinary vertex/fragment stage_in (MetalShaderTools). Likely used for tessellation/patch inputs (`air.patch_input`, `air.patch_control_point_input` listed at metal_reflection.py:60-61). UNKNOWN. L.
* `air.arg_type_name` for stage-in members is the MSL type name (`float4`, `half4`, `float2`, `uint`...), not the LLVM type. Dump READ. H.

### F9 Early fragment tests, raster order, imageblock (adjacent items seen while grepping)
* `early_fragment_tests`: bare MDString appended to the function's metadata node after operand 2 (spirv2air :1518; crosstl decorations.py:116 treats it as "early_fragment_tests", no `air.` prefix). READ. M. Not in dumps.
* `air.raster_order_group, i32 N` appears in dumps on an `air.texture` node (READ, 1 module) and in spirv2air on buffers/render targets (:964, :990). READ.

----------------------------------------------------------------------
## 4. Mapping table (proposal unless the "evidence" column says dump or two+ allowed sources)

SPIR-V capability/extension/Vulkan-feature columns come from SPIRV-Headers@f6f78c7 `include/spirv/unified1/spirv.core.grammar.json` (READ) plus Vulkan knowledge (INFER; Vulkan spec not re-fetched).

| AIR attribute / token (stage, direction) | IR value type | MSL attr / type | SPIR-V BuiltIn / Decoration | SPIR-V capability (ext) | Vulkan feature | status / evidence |
|---|---|---|---|---|---|---|
| `air.position` (vertex out) | float4 | `[[position]]` | BuiltIn Position (+ Decoration Invariant if `air.invariant`) | Shader | none | EVIDENCED (dump) ; invariant = proposal (1 source) |
| `air.position` + `air.center` `air.no_perspective` (fragment in) | float4 | `[[position]]` | BuiltIn FragCoord | Shader | none | EVIDENCED (dump) |
| `air.vertex_id` (vertex in) | i32 | `[[vertex_id]]` uint | BuiltIn VertexIndex | Shader | none | EVIDENCED (dump, 10 nodes); includes base vertex per MSL spec |
| `air.instance_id` | i32 | `[[instance_id]]` uint | BuiltIn InstanceIndex | Shader | none | proposal (3 sources: LuisaCompute, spirv2air-by-analogy, crosstl; 0 in dumps) |
| `air.base_vertex` | i32 | `[[base_vertex]]` uint | BuiltIn BaseVertex | DrawParameters (SPV_KHR_shader_draw_parameters, SPIR-V 1.3) | shaderDrawParameters (Vulkan 1.1) | proposal (spirv2air, crosstl; SPIRV-Cross names) |
| `air.base_instance` | i32 | `[[base_instance]]` uint | BuiltIn BaseInstance | DrawParameters | shaderDrawParameters | proposal (LuisaCompute, crosstl) |
| `air.point_size` (vertex out) | float | `[[point_size]]` | BuiltIn PointSize | Shader | none (geometry/tess: shaderTessellationAndGeometryPointSize) | EVIDENCED (dump) |
| `air.point_coord` (fragment in) | `<2 x float>` | `[[point_coord]]` float2 | BuiltIn PointCoord | Shader | none | EVIDENCED (dump) |
| `air.front_facing` (fragment in) | i1 | `[[front_facing]]` bool | BuiltIn FrontFacing | Shader | none | proposal (LuisaCompute, spirv2air, crosstl, kaleido3d) |
| `air.sample_id` (fragment in) | i32 | `[[sample_id]]` uint | BuiltIn SampleId | SampleRateShading | sampleRateShading | proposal (spirv2air, crosstl); forces per-sample execution |
| `air.sample_mask_in` (fragment in) | i32 | `[[sample_mask]]` uint | BuiltIn SampleMask (Input, array of 1) | Shader | none | proposal (spirv2air, crosstl, kaleido3d) |
| `air.sample_mask` (fragment out) | i32 | `[[sample_mask]]` uint | BuiltIn SampleMask (Output) | Shader | none | proposal (spirv2air, crosstl) |
| `air.render_target_array_index` (vertex out; also fragment in) | i32 (MSL uint) | `[[render_target_array_index]]` | BuiltIn Layer | ShaderLayer (Vulkan 1.2) or ShaderViewportIndexLayerEXT (SPV_EXT_shader_viewport_index_layer); Geometry in geometry stage | shaderOutputLayer (1.2) / VK_EXT_shader_viewport_index_layer; fragment input read: Geometry cap historically | proposal (crosstl, kaleido3d, SPIRV-Cross names) |
| `air.viewport_array_index` | i32 | `[[viewport_array_index]]` uint | BuiltIn ViewportIndex | ShaderViewportIndex (1.2) / ShaderViewportIndexLayerEXT / MultiViewport (geometry) | shaderOutputViewportIndex + multiViewport | proposal (same) |
| `air.amplification_id` | ? | `[[amplification_id]]` ushort/uint | BuiltIn ViewIndex ? | MultiView (SPV_KHR_multiview, 1.3) | multiview | LOW: only crosstl name; semantic mismatch possible |
| `air.depth` + `air.depth_qualifier` `air.any` (fragment out) | float | `[[depth(any)]]` | BuiltIn FragDepth + ExecutionMode DepthReplacing | Shader | none | proposal, strong (LuisaCompute, spirv2air, LLAIR) |
| `air.depth_qualifier air.greater` | float | `[[depth(greater)]]` | FragDepth + DepthReplacing + ExecutionMode DepthGreater | Shader | none | same |
| `air.depth_qualifier air.less` | float | `[[depth(less)]]` | FragDepth + DepthReplacing + ExecutionMode DepthLess | Shader | none | same |
| (no AIR tag) DepthUnchanged | | | ExecutionMode DepthUnchanged | Shader | none | SPIRV-Cross ignores it (picks `any` when neither greater nor less) /spirv_msl.cpp:18795-18801 |
| `air.stencil` (fragment out) | i32 | `[[stencil]]` uint | BuiltIn FragStencilRefEXT + ExecutionMode StencilRefReplacingEXT | StencilExportEXT (SPV_EXT_shader_stencil_export) | VK_EXT_shader_stencil_export | proposal (spirv2air; SPIRV-Cross) |
| `air.center` `air.perspective` (fragment_input) | per type | default / `center_perspective` | no decoration (default) | Shader | none | EVIDENCED (dump, 48) |
| `air.center` `air.no_perspective` | | `center_no_perspective` | Decoration NoPerspective | Shader | none | EVIDENCED for `[[position]]` (dump); user varyings: proposal (LLAIR, LuisaCompute, spirv2air) |
| `air.centroid` `air.perspective` | | `centroid_perspective` | Decoration Centroid | Shader | none | proposal (3 sources) |
| `air.centroid` `air.no_perspective` | | `centroid_no_perspective` | Centroid + NoPerspective | Shader | none | proposal |
| `air.sample` `air.perspective` | | `sample_perspective` | Decoration Sample | SampleRateShading | sampleRateShading | proposal; makes fragment code per-sample |
| `air.sample` `air.no_perspective` | | `sample_no_perspective` | Sample + NoPerspective | SampleRateShading | sampleRateShading | proposal |
| `air.flat` (single token) | int or float | `flat` | Decoration Flat | Shader | none | proposal (3 sources); ALL integer varyings need it |
| `air.vertex_output` / `air.fragment_input` + `generated(...)` or `user(...)` | | struct member / `[[user(name)]]` | Decoration Location (match by identity string, not number) | Shader | none | EVIDENCED (dump) for generated(...); `user(...)` proposal |
| `air.render_target RT SECOND` (fragment out) | | `[[color(RT), index(SECOND)]]` | Location=RT, Index=SECOND | Shader | dualSrcBlend if SECOND=1 | EVIDENCED for SECOND=0 (dump); dual-source UNPROVEN |
| `air.clip_distance` + `air.clip_distance_array_size N` | `[N x float]` | `[[clip_distance]]` | BuiltIn ClipDistance | ClipDistance | shaderClipDistance | proposal (spirv2air, crosstl) |
| `air.barycentric_coord` + `air.center` + perspective/no_perspective | `<3 x float>` | `[[barycentric_coord]]` | BuiltIn BaryCoordKHR / BaryCoordNoPerspKHR | FragmentBarycentricKHR | fragmentShaderBarycentric | proposal (LuisaCompute, spirv2air) |
| `air.thread_position_in_grid` | uint/ushort (2/3-vec) | same | BuiltIn GlobalInvocationId | none (core) | none | proposal, strong (vertex-language/air, SPIRV-Cross, LuisaCompute) |
| `air.thread_position_in_threadgroup` | | same | LocalInvocationId | none | none | same |
| `air.threadgroup_position_in_grid` | | same | WorkgroupId | none | none | EVIDENCED (dump) |
| `air.threads_per_threadgroup` | | same | WorkgroupSize (constant) | none | none | EVIDENCED (dump) ; tail-group caveat F1 |
| `air.threadgroups_per_grid` | | same | NumWorkgroups | none | none | proposal (3 sources) |
| `air.thread_index_in_threadgroup` | scalar | same | LocalInvocationIndex | none | none | proposal (3 sources) |
| `air.thread_index_in_simdgroup` | i16/i32 | same | SubgroupLocalInvocationId | GroupNonUniform | subgroup (VK 1.1 subgroupProperties) | EVIDENCED (dump) |
| `air.simdgroup_index_in_threadgroup` | | same | SubgroupId | GroupNonUniform | subgroup | EVIDENCED (dump) |
| `air.simdgroups_per_threadgroup` | | same | NumSubgroups | GroupNonUniform | subgroup | proposal (3 sources) |
| `air.threads_per_simdgroup` OR call `@air.get_simdgroup_size.i16` | | same | SubgroupSize | GroupNonUniform | subgroup | attribute: proposal (LuisaCompute, vertex-language/air); intrinsic: EVIDENCED (dump) |

Notes on the table
1. Capability names for GlobalInvocationId/WorkgroupId/etc. are `None` in the grammar JSON (core compute builtins); SubgroupSize etc. list Kernel, GroupNonUniform, SubgroupBallotKHR; for Vulkan use GroupNonUniform.
2. Vulkan feature names marked INFER were not re-read from the Vulkan spec in this session.
3. Layer/ViewportIndex: the grammar lists Geometry, ShaderLayer, ShaderViewportIndexLayerEXT, MeshShading* for Layer and MultiViewport, ShaderViewportIndex, ShaderViewportIndexLayerEXT, MeshShading* for ViewportIndex (READ, SPIRV-Headers). A vertex-stage writer on Vulkan 1.2 needs ShaderLayer/ShaderViewportIndex; on 1.0/1.1 it needs the EXT capability.

----------------------------------------------------------------------
## 5. Unknown / not evidenced (do not implement from these sources alone)

1. Real Apple-compiler output for: `air.instance_id`, `air.base_vertex`, `air.base_instance`, `air.front_facing`, `air.sample_id`, `air.sample_mask(_in)`, `air.render_target_array_index`, `air.viewport_array_index`, `air.depth`/`air.depth_qualifier`, `air.stencil`, `air.flat`/`air.centroid`/`air.sample` interpolation, `air.invariant`, `air.clip_distance`, `air.thread_position_in_grid` and the other compute built-ins not in the one dumped kernel. Everything for these rests on third-party AIR writers (LuisaCompute is the most trustworthy because it runs on Apple hardware; spirv2air, crosstl and kaleido3d are secondary). Recommended verification, requires macOS `xcrun metal -S -emit-llvm`, which is not available here: compile one MSL file using each attribute and diff against section 3.
2. `air.amplification_id`: operand layout, how `amplification_count` is declared, relation to SPIR-V ViewIndex.
3. Dual-source blending (`air.render_target RT 1`) and `[[color(m)]]` input shapes beyond what the dump shows.
4. Whether `air.stage_in` is ever emitted (tessellation/patch only?). Not in dumps; not a plain vertex/fragment record.
5. Vertex-input metadata (`air.vertex_input`) vs driver-generated `air.vertexFetchFunction`: dumps show only the fetch-function path; `air.vertex_input` shape is from LuisaCompute/LLAIR only and the older LLAIR reader/writer layout differs slightly from the current ABI (identifier position).
6. Interaction rules: per-sample shading is triggered by `air.sample_id`, `air.sample` interpolation or sample_mask input (MSL spec for `sample_*` interpolation, READ summariser; SPIRV-Cross :1916-1919). Exact AIR/pipeline requirements for per-sample inputs (e.g. whether the pipeline descriptor must enable sample shading) are not shown in any source.
7. `position` for `FragCoord` with `air.center air.no_perspective` is the only interpolation Metal allows (MSL spec Sec. 5.4) so no `Centroid`/`Sample` handling is needed for FragCoord. READ (summariser). SPIRV-Cross :14463 does the same.
8. MSL spec facts above came through a summarising fetch (tables paraphrased); re-read the PDF pages (Tables 5.2, 5.4, 5.5, 5.7, 5.8; Sec. 5.4) before quoting.
