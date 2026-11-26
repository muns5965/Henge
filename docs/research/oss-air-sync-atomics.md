# AIR -> SPIR-V: sync, atomics, SIMD-group, discard, texture forms (census X08 / X12)

Read-only research. No code changed. Clones (shallow) were deleted afterwards.
Tags: READ = seen verbatim in a licence-clean source; INFER = derived; UNVERIFIED = no allowed source.

## 0. Sources and licences

| Source | Commit | Licence (checked) | Used for |
|---|---|---|---|
| vertex-language/air (`intrinsic.go`, `intrinsic_test.go`, `kernel.go`, `README.md`, `testdata/kernels/*.ll`) | 3d277d16858a7141eb021fd614f021b595c13978 (HEAD = pinned commit) | MIT (`LICENSE`, "Copyright (c) 2026 Netangular Technologies") | exact AIR names and operand lists |
| KhronosGroup/SPIRV-Cross (`spirv_msl.cpp`, `spirv_msl.hpp`, `spirv_glsl.cpp`, `spirv.hpp`) | f0edde12d69ea7a498da14af05ec14f390b07c98 | file header `SPDX: Apache-2.0 OR MIT`; `LICENSE` = Apache-2.0 | SPIR-V -> MSL reverse mapping, numeric enum values |
| This repo (libapplegfx-vulkan, worktree `/tmp/wt-lintex` @d14341b) | - | own code | what the translator and the capability probe already do |
| Vulkan / SPIR-V specs, MSL spec | - | public | semantics (cited from knowledge, no fetch) |

Not used: Mesa and MoltenVK. Neither contains AIR intrinsic names (they consume or produce SPIR-V, MSL or NIR), so they add nothing here.
A web search for AIR texture names returned only repos whose licence I did not check (CrossGL/crosstl, dttdrv/mullion, ben-clayton/misc). I did not open them. They are on the skip list.
Forbidden sources were not touched.

Limit of vertex-language/air: it only declares the intrinsics for compute kernels (barrier, atomics, simd, `texture2d<float>`).
It has nothing on discard, quad_*, compare/gather/cube/array/1D/MSAA textures, or fragment-stage operations.
Those items are UNVERIFIED (sections 6 and 7).

## 1. Anchor: what the two dumps contain

Method: `strings`/`grep -a -o -F` over every `*.bin` in `/tmp/x231501/dump` (153 bins) and `/tmp/x230109/dump` (83 bins). Bitcode strtab names are concatenated, so these are substring counts. I cross-checked them with `/tmp/census_work/protos1.txt` and `decl_bc2.txt`. `llvm-dis` is not installed, and the `inv.c` reader tool lists functions and instruction codes only.

| name or pattern | x231501 | x230109 |
|---|---|---|
| `air.wg.barrier`, `air.simdgroup.barrier`, `air.atomic.*` (all forms), `air.atomic.fence` | 0 | 0 |
| `air.discard_fragment` | 0 | 0 |
| `air.quad_*` | 0 | 0 |
| `air.sample_compare*`, `sample_depth`, `gather*`, `*_cube`, `*_1d`, `*_2d_array`, `*_ms`, `get_num_samples` (as AIR intrinsic) | 0 | 0 |
| `air.simd_*` | 1 (`air.simd_shuffle_and_fill_down.v4f16`) | 1 (same module) |
| `air.get_simdgroup_size.i16` | 1 | 1 |
| metadata keys `air.thread_index_in_simdgroup`, `air.simdgroup_index_in_threadgroup`, `air.threads_per_threadgroup`, `air.threadgroup_position_in_grid` | 1 each | 1 each |
| `cube` | 4 hits, all the texture3d argument names `color_cube0/1` (LUT), not an intrinsic | same |

- Every simd/simdgroup hit is in one module: `lib_c5114484c9521c2e.bin`, function `tile_simd_blur_15`. It is the same file in both dumps.
- That module also uses `air.read_texture_2d.i16.v4f16`, `air.fma.v4f16` and `air.store.implicit_imageblock.v4f16`.
- There is no `addrspace(3)` global anywhere. `address_space` occurs 36 times, but only as argument metadata for device/constant buffers (census X12).
- Result: X08 and X12 are not exercised by captured guest content except `simd_shuffle_and_fill_down`. Everything below is for completeness and has no dump-level ground truth.

Already in the translator (worktree `/tmp/wt-lintex`, `src/air2spv/translate_function.c`): `get_simdgroup_size`, `simd_shuffle{,_down,_up,_xor}`, `simd_shuffle_and_fill_{down,up}`, the logical-32 emulation (`t_simd_emul`), and `air.wg.barrier` -> `OpControlBarrier` (header comment around line 1962).
Missing: atomics, `air.atomic.fence`, `air.simdgroup.barrier`, simd reductions, ballot, any/all, quad, discard, addrspace-3 variables.
The existing texture-sample handler ignores the option operands, so bias, lod, gradient and offset are not honoured.

## 2. Threadgroup memory (addrspace 3 -> Workgroup)

AIR forms (READ: vertex-language `README.md` lines 22-25, 194, 207, 256; `testdata/kernels/reduce_sum.ll`; MIT):
- Address spaces: `0` thread, `1` device, `2` constant, `3` threadgroup.
- Static threadgroup variable (MSL `threadgroup T x[N]` in the kernel body) is a module global: `@partial = internal addrspace(3) global [32 x float] undef, align 4`. Accessed through `getelementptr` into `float addrspace(3)*`.
- Dynamic threadgroup buffer is a kernel argument `T addrspace(3)*` with metadata `air.buffer, air.location_index i, air.address_space 3` (MSL `threadgroup T* p [[threadgroup(i)]]`). Its size comes from `setThreadgroupMemoryLength` at dispatch.

SPIR-V mapping (INFER from the SPIR-V spec; SPIRV-Cross `spirv_msl.cpp:9486`, `11872` read for the reverse direction, `Workgroup` -> `threadgroup`):
- Global `addrspace(3)`: `OpVariable %ptr_Workgroup_T Workgroup` (storage class 4). `undef` initializer: emit no initializer. Vulkan allows an initializer on Workgroup variables only with `VK_KHR_zero_initialize_workgroup_memory`.
- Pointers into it: `OpTypePointer Workgroup`. `OpAccessChain` replaces the GEP.
- Dynamic threadgroup argument: SPIR-V has no per-dispatch size. Declare the array with a specialization-constant length (`OpSpecConstant`, own SpecId) and set it at pipeline creation. The size must therefore be known at pipeline build time. UNVERIFIED how the guest-protocol tile/compute path carries the length.
- No capability is needed. No Vulkan feature is needed.
- Limit: `maxComputeSharedMemorySize` (Vulkan minimum 16 KiB; Metal threadgroup memory up to 32 KiB). Adreno 840 limit not probed in `src/vulkan/` and not logged. UNVERIFIED.
- Pointer passed to a helper function in addrspace(3): function-parameter pointers to Workgroup are allowed in logical addressing. This is spec knowledge, medium confidence.
- The repo's `variablePointersStorageBuffer` is irrelevant to Workgroup pointers.
- Atomics on the memory use scope Workgroup (section 4).

## 3. Barriers and fences

### 3.1 AIR forms (READ: vertex-language `intrinsic.go:141-157`, `intrinsic_test.go:46-47`, `reduce_sum.ll:23,51`; MIT)

| AIR call | operands | note |
|---|---|---|
| `void @air.wg.barrier(i32 flags, i32 1)` | `flags` = MSL mem_flags | `threadgroup_barrier(mem_threadgroup)` is `(2, 1)` |
| `void @air.simdgroup.barrier(i32 flags, i32 1)` | same | `simdgroup_barrier` |
| `void @air.atomic.fence(i32 flags, i32 order, i32 scope)` | MSL >= 3.2 only, `atomic_thread_fence` | |

- flags: `mem_none=0`, `mem_device=1`, `mem_threadgroup=2`, `mem_texture=4` (OR-able).
- Fence order: `0` = relaxed, `5` = seq_cst (only these two seen).
- Fence scope: `1` = threadgroup, `2` = device. Values for thread and simdgroup scope are not shown anywhere. UNVERIFIED.
- The second operand of the two barriers is the constant 1 in every pinned case. Its meaning is unknown (probably a scope). Do not depend on it. Pick the execution scope from the function name.
- Attributes: barriers and simd ops are `convergent`. Fence is not.

### 3.2 SPIR-V mapping (INFER; enum values READ in SPIRV-Cross `spirv.hpp`: Scope 0 CrossDevice, 1 Device, 2 Workgroup, 3 Subgroup, 4 Invocation; MemorySemantics Acquire 0x2, Release 0x4, AcquireRelease 0x8, SeqCst 0x10, Uniform 0x40, Subgroup 0x80, Workgroup 0x100, CrossWorkgroup 0x200, Image 0x800)

`OpControlBarrier` is opcode 224. `OpMemoryBarrier` is opcode 225.
- `air.wg.barrier(f, _)`:
  - `OpControlBarrier Exec=Workgroup(2), Mem=(f&1 ? Device(1) : Workgroup(2)), Sem=AcquireRelease(0x8) | (f&2 ? 0x100) | (f&1 ? 0x40) | (f&4 ? 0x800)`.
  - The existing translator already does the Workgroup/Workgroup form.
  - Reverse check, SPIRV-Cross `spirv_msl.cpp:11385-11436`: flag device <- Uniform|CrossWorkgroup, threadgroup <- Workgroup|Subgroup, texture <- Image.
- `air.simdgroup.barrier(f, _)`: `OpControlBarrier Exec=Subgroup(3), Mem=Subgroup(3), Sem=AcquireRelease | SubgroupMemory(0x80) | the same bits as above`. `f == 0` is a pure execution barrier (Sem 0 is valid).
- `air.atomic.fence(f, order, scope)`:
  - `OpMemoryBarrier Mem=scope, Sem=<bits as above> | (order==5 ? AcquireRelease : 0)`.
  - Scope map: 1 -> Workgroup(2), 2 -> Device(1).
  - For order `5` use AcquireRelease (0x8). Do not use SequentiallyConsistent (0x10), which Vulkan treats as AcquireRelease anyway (medium confidence).
  - SPIRV-Cross `spirv_msl.cpp:11450-11475` maps any acquire/release/seq_cst to `memory_order_seq_cst` and everything else to relaxed. It maps Subgroup -> `thread_scope_simdgroup` and Invocation -> `thread_scope_thread`.
- Capability: none (Shader). Vulkan feature: none. The Vulkan memory model is not needed.
- Edge cases:
  - A barrier in non-uniform control flow is undefined in both APIs.
  - Adreno 840 subgroup is 64 lanes. A logical 32-lane simdgroup barrier mapped to a 64-lane Subgroup barrier is a superset and is safe, but it requires both logical halves to arrive (a divergent-halves kernel would deadlock/UB). Low risk, UNVERIFIED.
  - `OpMemoryBarrier` must be in a compute/fragment function that allows it. `air.atomic.fence` in a fragment shader is fine.

## 4. Atomics

### 4.1 AIR forms (READ: vertex-language `intrinsic.go:393-490`, `intrinsic_test.go:128-195` pinned against xcrun by the library's own test; I could not run that test; MIT)

Name: `air.atomic.<space>.<op>[.<s|u>].<ty>`.
- `<space>` = `global` (pointer addrspace 1, device) or `local` (pointer addrspace 3, threadgroup).
- `<ty>` = `i32` or `f32` only. The library notes 64-bit atomics are not built and half/vector are not covered.
- Trailing operands `(i32 order, i32 scope, i1 volatile)`: `order` is always 0 (relaxed), `scope` is `2` for global and `1` for local, `volatile` is always `true`.

| MSL | AIR name | operands | returns |
|---|---|---|---|
| `atomic_load_explicit` | `air.atomic.{global,local}.load.{i32,f32}` | `(ptr, i32 order, i32 scope, i1 vol)` | element value |
| `atomic_store_explicit` | `...store.{i32,f32}` | `(ptr, value, order, scope, vol)` | void |
| `atomic_exchange_explicit` | `...xchg.{i32,f32}` | `(ptr, value, order, scope, vol)` | old value |
| `atomic_fetch_{add,sub,and,or,xor,min,max}_explicit` (int) | `...<op>.{s,u}.i32` e.g. `air.atomic.global.add.u.i32`, `air.atomic.local.add.u.i32`, `air.atomic.global.max.u.i32`, `air.atomic.global.xor.s.i32` | `(ptr, value, order, scope, vol)` | old value |
| `atomic_fetch_{add,sub}_explicit` (float) | `air.atomic.global.add.f32` | `(ptr, value, order, scope, vol)` | old value |
| `atomic_compare_exchange_weak_explicit` | `air.atomic.{global,local}.cmpxchg.weak.i32` | `(ptr, i32* expected [addrspace 0 alloca], i32 desired, i32 order_succ, i32 order_fail, i32 scope, i1 vol)` | see below |

- Float ops: add and sub only (plus load/store/xchg). No float min/max/and/or/xor.
- Integer sign: `.s`/`.u` only on RMW ops, not on load/store/xchg/cmpxchg. add and sub do not depend on the sign.
- Real dump sample (`testdata/kernels/histogram.ll:13`): `%6 = call i32 @air.atomic.global.add.u.i32(i32 addrspace(1)* %5, i32 1, i32 0, i32 2, i1 true)`.
- `reduce_sum.ll:42`: `call float @air.atomic.global.add.f32(float addrspace(1)* %out, float %14, i32 0, i32 2, i1 true)`.
- CONFLICT / UNVERIFIED: the library declares the cmpxchg return type as the element type ("returns what was found"). My recollection of real Apple output is `i1` (the MSL bool).
  - `TestIntrinsicsMatchXcrun` compares the full declared signature to xcrun, so the library may be right, but I could not run it (no xcrun here).
  - Translator rule: take the return type from the module's declaration. If it is `i1`, the result is the success flag. If it is `i32`, the result is the found value. In both cases write the found value to `*expected`.

### 4.2 SPIR-V mapping

Opcodes (READ in SPIRV-Cross `spirv.hpp`): AtomicLoad 227, AtomicStore 228, AtomicExchange 229, AtomicCompareExchange 230, IAdd 234, ISub 235, SMin 236, UMin 237, SMax 238, UMax 239, And 240, Or 241, Xor 242, AtomicFAddEXT 6035, AtomicFMinEXT 5614, AtomicFMaxEXT 5615.
Reverse direction READ in SPIRV-Cross `spirv_msl.cpp:10149-10260`.

Common operands: `Scope` from the AIR scope operand (2 -> Device(1), 1 -> Workgroup(2)), `Semantics` = 0 (Relaxed; `memory_order_relaxed` is the only order Metal takes for these, SPIRV-Cross `get_memory_order` at `spirv_msl.cpp:11992-11994`). The `volatile` operand is ignored (no Volatile memory operand needed).
- Storage class: `global` -> pointer into StorageBuffer (or PhysicalStorageBuffer if the library's BDA path is used); `local` -> Workgroup.

| AIR | SPIR-V |
|---|---|
| `load.i32` | `OpAtomicLoad %u32 ptr scope sem` |
| `store.i32` | `OpAtomicStore ptr scope sem value` |
| `xchg.i32` | `OpAtomicExchange %u32 ptr scope sem value` |
| `add.{s,u}.i32` | `OpAtomicIAdd` |
| `sub.{s,u}.i32` | `OpAtomicISub` |
| `and/or/xor` | `OpAtomicAnd/Or/Xor` |
| `min.s/max.s` | `OpAtomicSMin/SMax` |
| `min.u/max.u` | `OpAtomicUMin/UMax` |
| `cmpxchg.weak.i32` | `cmp = OpLoad expected_ptr`; `old = OpAtomicCompareExchange %u32 ptr scope semEq=0 semNeq=0 Value=desired Comparator=cmp`; `OpStore expected_ptr old`; success = `OpIEqual old cmp` |

- Operand order for CompareExchange: SPIR-V takes Value (the desired) before Comparator. READ in SPIRV-Cross `spirv_msl.cpp:10160-10173`: `ops[6]`=val, `ops[7]`=comp.
- Using the strong CAS for a weak CAS is legal (weak may fail spuriously, strong does not).
- SPIRV-Cross itself wraps weak CAS in a loop for the reverse direction (`spirv_msl.cpp:11883-11935`).
- Float ops, `air.atomic.*.add.f32`:
  - Preferred: `OpAtomicFAddEXT` (6035) with capability `AtomicFloat32AddEXT` (6033) and extension `SPV_EXT_shader_atomic_float_add`. Vulkan feature (`VK_EXT_shader_atomic_float`): `shaderBufferFloat32AtomicAdd` for global, `shaderSharedFloat32AtomicAdd` for local.
  - `sub.f32` = `OpFNegate` then `OpAtomicFAddEXT`.
  - Fallback with no extra feature: CAS loop on a uint view of the same memory (`OpBitcast` value, `OpAtomicCompareExchange`, loop until success). This needs the buffer to be declared as `uint` (logical addressing cannot bitcast a pointer), so the translator must choose the buffer element type up front.
- Float load/store/xchg: `OpAtomicLoad/Store/Exchange` on float needs `shaderBufferFloat32Atomics` / `shaderSharedFloat32Atomics`. Alternatively use the uint view. UNVERIFIED which of these Vulkan requires; medium confidence.
- Features already required by the repo: `fragmentStoresAndAtomics` and `vertexPipelineStoresAndAtomics` are probed and enabled (`src/vulkan/instance.c:640-643`), which covers atomics in fragment and vertex stages. Compute atomics on buffers need nothing extra.
- Adreno 840: the capability probe has NO bit for atomic float (`LAGFX_VK_CAP_*` list in `src/vulkan/caps.h` has none) and no `VkPhysicalDeviceShaderAtomicFloatFeaturesEXT` query in `instance.c`. Exposure of float atomics on Adreno 840 is therefore UNVERIFIED. Use the CAS fallback unless the probe is added.
- 64-bit atomics (`Int64Atomics`, cap 12): no AIR form seen in allowed sources. Skip.
- SPIRV-Cross MSL throws on 64-bit atomics (`spirv_msl.cpp:11728`), consistent with Metal having none at that time.

## 5. SIMD-group operations (compute, `air.simd_*`)

### 5.1 AIR forms (READ: vertex-language `intrinsic.go:285-391`, `intrinsic_test.go:96-127`; MIT)

Name: `air.simd_<op>.<suffix>`, attribute `convergent`.
- Suffix: floats `f32`/`f16` (vectors `v4f32`); integers `s.<iN>` or `u.<iN>`, e.g. `air.simd_max.s.i32`, `air.simd_sum.u.i32`.
- Lane operands are `i16` (the library truncates or zero-extends to ushort).

| MSL | AIR name (example) | operands |
|---|---|---|
| `simd_sum`, `simd_product`, `simd_min`, `simd_max` | `air.simd_sum.f32`, `air.simd_max.s.i32` | `(x)` |
| `simd_prefix_inclusive_sum`, `simd_prefix_exclusive_sum` | `air.simd_prefix_inclusive_sum.f32` | `(x)` |
| `simd_broadcast_first` | `air.simd_broadcast_first.f32` | `(x)` |
| `simd_broadcast` | `air.simd_broadcast.f32` | `(x, i16 lane)` |
| `simd_shuffle`, `_xor`, `_up`, `_down` | `air.simd_shuffle.f32`, `air.simd_shuffle_xor.u.i32` | `(x, i16 lane\|mask\|delta)` |
| `simd_ballot` | `air.simd_ballot.i64` | `(i1 pred)` -> `i64` |
| `simd_all`, `simd_any` | `air.simd_all`, `air.simd_any` | `(i1)` -> `i1` (no type suffix) |
| `simd_is_first` | `air.simd_is_first` | `()` -> `i1` |

- Seen in the dumps (not in the library): `air.simd_shuffle_and_fill_down.v4f16`, `air.get_simdgroup_size.i16` (x231501 and x230109, module `tile_simd_blur_15`).
- Names that exist in MSL but are NOT in any allowed AIR source: `simd_prefix_*_product`, `simd_and/or/xor`, `simd_shuffle_rotate_up/down`, `simd_active_threads_mask`, `simd_is_helper_thread`, `simd_shuffle_and_fill_up`, and the whole `quad_*` family. Plausible AIR spelling is `air.<msl_name>.<suffix>`, but that is INFER, low confidence. SPIRV-Cross lists the MSL names (`spirv_msl.cpp:18233-18345`, `6943-7485`).
- The `air.get_simdgroup_size` and `air.thread_index_in_simdgroup` builtins map to `SubgroupSize` / `SubgroupLocalInvocationId` (already done).

### 5.2 SPIR-V mapping (INFER; reverse mapping READ in SPIRV-Cross `spirv_msl.cpp:18115-18345`)

All need SPIR-V >= 1.3, Scope = Subgroup (3), capability `GroupNonUniform` (61) at minimum.

| AIR | SPIR-V | capability | Vulkan `VK_SUBGROUP_FEATURE_*` |
|---|---|---|---|
| `simd_sum/product` | `OpGroupNonUniform{I,F}{Add,Mul}` Reduce(0) | Arithmetic (63) | ARITHMETIC |
| `simd_min/max` | `OpGroupNonUniform{S,U,F}{Min,Max}` Reduce | Arithmetic | ARITHMETIC |
| `simd_prefix_inclusive_*` | same ops with InclusiveScan(1) | Arithmetic | ARITHMETIC |
| `simd_prefix_exclusive_*` | same ops with ExclusiveScan(2) | Arithmetic | ARITHMETIC |
| `simd_shuffle` | `OpGroupNonUniformShuffle` (345) | Shuffle (65) | SHUFFLE (probed) |
| `simd_shuffle_xor` | `OpGroupNonUniformShuffleXor` (346) | Shuffle | SHUFFLE (probed) |
| `simd_shuffle_up/down` | `ShuffleUp` (347) / `ShuffleDown` (348) | ShuffleRelative (66) | SHUFFLE_RELATIVE (probed) |
| `simd_broadcast` | `OpGroupNonUniformBroadcast` (337); lane must be a constant in Vulkan, so use `Shuffle` when it is not | Ballot (64) / Shuffle | BALLOT / SHUFFLE |
| `simd_broadcast_first` | `OpGroupNonUniformBroadcastFirst` (338) | Ballot (64) | BALLOT |
| `simd_ballot` -> i64 | `OpGroupNonUniformBallot` (339) -> `uvec4`; i64 = `(u64)v.x \| (u64)v.y << 32` | Ballot | BALLOT |
| `simd_all` / `simd_any` | `OpGroupNonUniformAll` (334) / `Any` (335) | Vote (62) | VOTE |
| `simd_is_first` | `OpGroupNonUniformElect` (333) | GroupNonUniform (61) | BASIC |

- SPIRV-Cross reverse for ballot (`spirv_msl.cpp:7010-7017`): MSL `simd_ballot` is a 64-bit `simd_vote` on macOS (32-bit on iOS). It converts it to `uint4(lo, hi, 0, 0)`.
- Metal has no clustered-op fallback needs here, but SPIRV-Cross's `spvClustered` template uses `simd_shuffle_xor` trees (`spirv_msl.cpp:7340-7400`). That is a pattern for a shuffle-only fallback of reductions.

### 5.3 Probe status and Adreno 840

READ in this repo (`src/vulkan/instance.c:573-590`, `caps.c:137-139`): the only subgroup capabilities probed are BASIC (`LAGFX_VK_CAP_SUBGROUP`), SHUFFLE and SHUFFLE_RELATIVE, all for the COMPUTE stage only.
Not probed: VOTE, ARITHMETIC, BALLOT, QUAD, CLUSTERED, any fragment-stage subgroup support, ROTATE.
Adreno 840 log (`/tmp/x231501/logs/g7_lagfx_1795702501625.txt`):
- `phys device 'Adreno (TM) 840' ... api=1.4.295`.
- `g7-gpu-feat: subgroup size=64 min=64 max=64 subgroupSizeControl=1 requiredSubgroupSizeStages=0xe0 computeFullSubgroups=1 pin32=no`.
- `g7-gpu-caps: enabled=[int64,int16,int8,float16,sb8,sb16,varptr_sb,varptr,spirv14,subgroup,sg_shuffle,sg_shuffle_rel]`.
- So BASIC + SHUFFLE + SHUFFLE_RELATIVE are confirmed for compute. Everything else in the table is UNVERIFIED on the device. Vulkan 1.4 may mandate more subgroup operations in compute, from memory of the 1.4 requirements table, medium-low confidence. Add probes before relying on it.
- Min and max are both 64, so the 32-wide pin is impossible and the logical-32 emulation path is the only one.

### 5.4 Edge cases: Apple 32-wide simdgroup on a 64-lane Adreno subgroup (INFER)

The repo's emulation (`t_simd_emul = 32`, `translate_function.c:2041-2060`, `3757-3760`) remaps lane id, size and shuffle indices. Reductions, scans, votes and ballots over the whole subgroup would be WRONG when only 32 lanes are meant. Needed per op:
- `simd_sum/min/max/product` (Reduce): `ClusteredReduce` with ClusterSize 32 (`GroupNonUniformClustered` cap 67, `VK_SUBGROUP_FEATURE_CLUSTERED`, not probed), or a shuffle-xor butterfly of 5 steps with `lane ^ {16,8,4,2,1}` (only needs the already-probed Shuffle).
- prefix scans: a whole-subgroup Inclusive/ExclusiveScan is wrong for the upper half (it would include the lower half's lanes). Use a shuffle-up Hillis-Steele loop, with a mask `(lane & 31) >= d`.
- `simd_ballot`: take `v.x` for logical group 0 or `v.y` for group 1 (`base = lane & ~31`) and zero-extend to i64, because Metal's mask covers 32 lanes here.
- `simd_all/any`: derive from the ballot masked to the logical group, instead of `OpGroupNonUniformAll/Any` over 64 lanes.
- `simd_broadcast_first` / `simd_is_first`: first active lane within the logical group = `findLSB(masked ballot)`. `Elect` picks the first lane of all 64 and is wrong for the upper half.
- `quad_*`: Vulkan quads are 4 aligned lanes (`lane & ~3`), the same as Metal, so no emulation is needed. Quad is `OpGroupNonUniformQuadBroadcast` (365) / `QuadSwap` (366), cap `GroupNonUniformQuad` (68), `VK_SUBGROUP_FEATURE_QUAD`. A `quad_sum` is `x + swap(h) + swap(v)`, or ClusteredReduce with size 4. In Vulkan, quad ops in the fragment stage need the stage in `subgroupSupportedStages`, which is not probed.
- Out-of-range shuffle indices (`>= size`, or the up/down beyond the edge) are undefined in Vulkan. Metal gives the lane's own or an unspecified value, UNVERIFIED. Keep the existing select/fill logic.
- Any subgroup op in a divergent branch only sees the active lanes in both APIs.

## 6. Fragment discard (X12)

AIR name: UNVERIFIED. No allowed source lists it. Best INFER is `call void @air.discard_fragment()` with no operands (name from MSL `discard_fragment()`, convention `air.<msl_name>`). It is absent from both dumps. Treat as low-medium confidence.

Metal semantics (READ, SPIRV-Cross `spirv_msl.cpp:1756-1765`, `10845-10848`, `spirv_msl.hpp:485-497`): from MSL 2.3, `discard_fragment()` has demote semantics (the thread continues as a helper invocation, derivatives stay valid). Metal is expected to suppress writes of discarded fragments. SPIRV-Cross only offers the opt-in `check_discarded_frag_stores` workaround for devices where this is buggy.

SPIR-V options:

| option | opcode | how it appears | needs |
|---|---|---|---|
| Demote (best match) | `OpDemoteToHelperInvocation` (5380), not a block terminator | cap `DemoteToHelperInvocation` (5379), ext `SPV_EXT_demote_to_helper_invocation` (core in SPIR-V 1.6) | Vulkan 1.3 feature `shaderDemoteToHelperInvocation` |
| Terminate | `OpTerminateInvocation` (4416), terminator | ext `SPV_KHR_terminate_invocation` (core in 1.6) | feature `shaderTerminateInvocation` |
| Legacy | `OpKill` (252), terminator | Shader | none (spec marks it deprecated from 1.6, medium confidence) |

- Adreno 840 reports api 1.4.295. Vulkan 1.3 requires both features, so they should be present (INFER, high confidence from the spec feature table). The repo does NOT probe or enable them: `VkPhysicalDeviceVulkan13Features` in `instance.c:521-526` sets only `dynamicRendering`, `synchronization2`, `subgroupSizeControl`. They must be added to `feat13` and to the probe. Without that, validation rejects the capability.
- Translator note: `air.discard_fragment` is a plain call in the middle of a block, so only the Demote form maps one-to-one. For the terminator forms, replace the rest of the block with the kill only if the remaining instructions are just the branch (the usual `if (c) discard_fragment();`).
- Early-fragment-tests interplay: if the shader writes depth or has storage writes, none of this changes the execution mode.
- Related reads: `OpIsHelperInvocationEXT` (5381) is the reverse of `simd_is_helper_thread()`. It is only relevant if AIR exposes `air.simd_is_helper_thread`. UNVERIFIED.

## 7. Texture forms (compare / gather / cube / array / 1D / MSAA)

No allowed source gives the AIR intrinsic names or operand orders for these. What is verified:
- Only 2D forms exist in vertex-language: `air.sample_texture_2d.v4f32`, `air.read_texture_2d.v4f32`, `air.write_texture_2d.v4f32`, `air.get_width_texture_2d`, `air.get_height_texture_2d`. The library README says the other shapes are "not built yet".
- Texture shape tags exist (`kernel.go:158-173`): `1d`, `2d`, `3d`, `cube`, `2d_array`, with opaque handle types `%struct._texture_1d_t`, `_texture_2d_t`, `_texture_3d_t`, `_texture_cube_t`, `_texture_2d_array_t`. By analogy the intrinsic suffix is `texture_<shape>` (INFER, medium).
- READ operand layout of `air.sample_texture_2d.v4f32` (`intrinsic.go`, `Sample`): `(tex, sampler, <2 x float> coord, i1 true, <2 x i32> offset(zero), i1 false, float 0, float 0, i32 0)` returning `{ <4 x float>, i8 }`. The meaning of each option operand is not documented there (INFER: coord-flag, offset, then bias/level/gradient selector and values, final i32 = kind). The repo handler currently ignores them, so bias, lod, gradient and offset forms are silently dropped. That is a correctness gap for any shader using `sample(..., level(x))`.
- `air.read_texture_2d.v4f32(tex, <2 x i32> coord, i32 lod, i32 1)` returns `{ <4 x float>, i8 }` (READ, README and `intrinsic.go`). `air.write_texture_2d.v4f32(tex, coord, <4 x float>, i32 lod, i32 2)`. Dump variant: `air.read_texture_2d.i16.v4f16` takes `i16` coordinates.

Recalled, NOT from an allowed source (UNVERIFIED, low confidence, do not code against them without a dump): `air.sample_depth_2d.f32`, `air.sample_compare_depth_2d.f32`, `air.gather_texture_2d.v4f32`, `air.gather_depth_2d.v4f32`, `air.gather_compare_depth_2d.v4f32`, `air.sample_texture_cube.v4f32`, `air.sample_texture_2d_array.v4f32`, `air.sample_texture_1d.v4f32`, `air.read_texture_2d_ms.v4f32`, `air.get_num_samples_texture_2d_ms`.

SPIR-V side for each, with the MSL argument order SPIRV-Cross relies on (it forwards to MSL `sample(s, coord, array, ...)`, `gather(s, coord, ..., component::x)`, `gather_compare(...)`, `spirv_msl.cpp:6800-6935`; INFER for the rest):
- compare: `OpImageSampleDrefImplicitLod` / `OpImageSampleDrefExplicitLod`. Depth-reference operand follows the coordinate (and the array index). The image must be a depth image (`Depth=1`) with a comparison sampler (`VkSamplerCreateInfo::compareEnable`).
- gather: `OpImageGather` (component operand) / `OpImageDrefGather`. No capability for plain gather. Non-constant offsets need `ImageGatherExtended` (25) and the `shaderImageGatherExtended` feature. SPIRV-Cross emulates it with four gathers when offsets are non-constant (`spirv_msl.cpp:6890-6930`).
- cube: `Dim=Cube`; the coordinate is `vec3`. Cube arrays need `ImageCubeArray` (34) / `SampledCubeArray` (45) and the `imageCubeArray` feature. SPIRV-Cross notes iOS lacks texturecube_array (`spirv_msl.cpp:6563`).
- array: `Arrayed=1`, array layer is the last coordinate component (`float` rounded to the nearest layer in Vulkan; Metal takes a separate uint operand). Convert to a float coordinate component.
- 1D: `Dim=1D`, capability `Sampled1D` (43) / `Image1D` (44).
- MSAA: `OpImageFetch`/`OpImageRead` with the `Sample` image operand, `MS=1`. Storage MS needs `StorageImageMultisample` (27); `ImageMSArray` (48) for arrays. `get_num_samples` -> `OpImageQuerySamples` (cap `ImageQuery` 50).
- Adreno 840 exposure of these Vulkan features (`shaderImageGatherExtended`, `imageCubeArray`, `shaderStorageImageMultisample`, `sampleRateShading`): not probed in `src/vulkan/`, no log line. UNVERIFIED.

## 8. Summary table: implementability now

| Item | AIR name/operands | Evidence | Implementable now |
|---|---|---|---|
| Threadgroup addrspace(3) globals | module `addrspace(3)` globals | READ MIT | yes, no feature; dynamic size needs spec constants |
| `air.wg.barrier` | `(flags, 1)` | READ MIT | already partly done |
| `air.simdgroup.barrier` | `(flags, 1)` | READ MIT | yes |
| `air.atomic.fence` | `(flags, order, scope)` | READ MIT | yes (MSL >= 3.2 only) |
| int atomics load/store/xchg/RMW | `(ptr, val, 0, scope, true)` | READ MIT | yes, core Vulkan |
| cmpxchg.weak | `(ptr, exp*, desired, 0, 0, scope, true)` | READ MIT, return type CONFLICT | yes, read the declared return type |
| float atomic add/sub | `air.atomic.*.add.f32` | READ MIT | via CAS fallback; native needs probe |
| simd reductions, scans, ballot, any/all | names READ | READ MIT | need probes for ARITHMETIC/BALLOT/VOTE and 64-lane emulation |
| quad_* | name UNVERIFIED | none | no |
| discard | name UNVERIFIED | none | after the 1.3 feature enable; name from a dump |
| compare/gather/cube/array/1D/MSAA | UNVERIFIED | none | no |

## 9. Items worth saving (for the main session)

- Adreno 840 runtime facts from the x231501 log: api 1.4.295, subgroup 64/64/64, `subgroupSizeControl=1`, `requiredSubgroupSizeStages=0xe0`, `computeFullSubgroups=1`, enabled caps `[int64,int16,int8,float16,sb8,sb16,varptr_sb,varptr,spirv14,subgroup,sg_shuffle,sg_shuffle_rel]`, missing `[ub8,pc8,ub16,pc16,io16]`.
- No memory-derived user facts were used.

## 10. Coordinator addendum

### 10.1 R07: root 0x42 CmdSynchronizeResources record size

Source: `src/handlers/sync/sync.c:19-33` in the worktree (/tmp/wt-lintex @d14341b). The comment says `+4 u32 range_count (8-byte records: {u64 gpa, u64 length})`. The bounds check divides the remaining payload by 8. A {u64,u64} record is 16 bytes, so the comment contradicts itself.

Own logs and dumps (READ):
- I scanned all 62 `g7_lagfx_*.txt` logs found on disk, including R1 and R2 (`/tmp/x231501/logs`, `/tmp/x230109/logs`). I searched for `op=0x0042` and for "CmdSynchronizeResources" text.
- 0 hits. The only root opcodes ever logged by the "diag cmd" lines are 0x30 (x20), 0x33 (x4), 0x38 (x4), 0x3a (x4). This matches census R07 ("0 occurrences").
- The dump directories hold no root command bytes.
- The 0x25 / `ref=0x42` line in R1 is a remove-object of task ref 0x42, which is unrelated.

Conclusion: there are no counts and no raw bytes. Neither the 8-byte reading nor the 16-byte reading is supported by our data. Both stay UNVERIFIED. Safest handling is to accept `range_count*8 <= payload-8` OR `range_count*16 <= payload-8` as valid, and not to dereference the ranges until a real payload is captured (the handler is a no-op that returns NOT_IMPLEMENTED anyway). If the payload is exactly `8 + 16*n` bytes with n = range_count, that settles it for 16.

### 10.2 Exact values: Metal operand -> SPIR-V Scope / Semantics

AIR operand values are READ from vertex-language/air (MIT, @3d277d16858a, `intrinsic.go:22-58`, `141-157`, `437-490`). SPIR-V enum values are READ from SPIRV-Cross `spirv.hpp`. The mapping itself is INFER.

AIR operands:
- mem_flags operand (barriers, fence): none=0, device=1, threadgroup=2, texture=4.
- Fence `order` operand: relaxed=0, seq_cst=5. Fence `scope` operand: threadgroup=1, device=2. Simdgroup/thread scope values: UNVERIFIED.
- Barrier second operand: 1 in every pinned case (meaning UNVERIFIED).
- Atomic trailing operands `(order, scope, volatile)`: order=0 (relaxed, success and failure alike for cmpxchg); scope=2 for `global`, 1 for `local`; volatile=true.

SPIR-V constants: Scope Device=1, Workgroup=2, Subgroup=3, Invocation=4. Semantics Acquire=0x2, Release=0x4, AcquireRelease=0x8, SeqCst=0x10, Uniform=0x40, Subgroup=0x80, Workgroup=0x100, CrossWorkgroup=0x200, Image=0x800.

| AIR call (operands carried) | Exec scope | Memory scope | Semantics |
|---|---|---|---|
| `air.wg.barrier(2,1)` (threadgroup mem) | Workgroup 2 | Workgroup 2 | 0x8 \| 0x100 = 0x108 |
| `air.wg.barrier(1,1)` (device mem) | 2 | Device 1 | 0x8 \| 0x40 = 0x48 |
| `air.wg.barrier(3,1)` (device+threadgroup) | 2 | Device 1 | 0x8 \| 0x40 \| 0x100 = 0x148 |
| `air.wg.barrier(4,1)` (texture) | 2 | Workgroup 2 (Device if combined with 1) | 0x8 \| 0x800 = 0x808 |
| `air.wg.barrier(0,1)` | 2 | Workgroup 2 | 0 (pure execution barrier) |
| `air.simdgroup.barrier(f,1)` | Subgroup 3 | Subgroup 3 | 0x8 \| 0x80 (if f&2) \| 0x40 (f&1) \| 0x800 (f&4); f=0 gives 0 |
| `air.atomic.fence(f, 0, 1)` | none (OpMemoryBarrier) | Workgroup 2 | bits as above, no ordering bit (relaxed -> 0, or 0x8 if you want a conservative fence) |
| `air.atomic.fence(f, 5, 1)` | none | Workgroup 2 | 0x8 \| bits (use AcquireRelease, not 0x10) |
| `air.atomic.fence(f, 5, 2)` | none | Device 1 | 0x8 \| bits |
| `air.atomic.global.*` (0,2,true) | none | Device 1 | 0 (Relaxed) |
| `air.atomic.local.*` (0,1,true) | none | Workgroup 2 | 0 (Relaxed) |
| cmpxchg success/fail order (0,0) | none | per space | Equal=0, Unequal=0 |

Caveats:
- For the barrier memory scope, SPIRV-Cross demotes Device to Workgroup when no device-class bits are present (`spirv_msl.cpp:11441-11448`). The same rule applies in this direction.
- An atomic with Relaxed semantics orders nothing. That is what Metal gives (SPIRV-Cross `get_memory_order` always returns `memory_order_relaxed`).
- If the atomic is used for synchronisation across workgroups, the shader still depends on a following barrier or fence.

### 10.3 Apple 32-wide vs Adreno 64-wide (summary)

- Adreno 840: subgroup size 64, min=max=64, not pinnable (READ in the R1 log). Metal simdgroup: 32.
- Operations the existing emulation already covers (READ in the repo): lane id, simdgroup size, shuffle, shuffle_up/down/xor, `shuffle_and_fill`, simdgroup index.
- Operations that need extra logic to be correct with 32 logical lanes in a 64-lane subgroup: reductions (sum/min/max/product), prefix scans, ballot (select the 32-bit half, zero-extend to i64), any/all (via masked ballot), broadcast_first, is_first, and simdgroup_barrier (safe superset). Details in section 5.4.
- `quad_*`: quad = 4 aligned lanes in both APIs, so no change.
- `simdgroup_barrier` over 64 lanes needs both logical halves to arrive; a kernel with divergent halves could hang. UNVERIFIED, low risk.
- Subgroup arithmetic, ballot, vote and quad capabilities are not probed, so Adreno exposure is UNVERIFIED (only basic, shuffle and shuffle_relative are confirmed for compute).
