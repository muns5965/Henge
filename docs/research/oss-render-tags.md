# ARM64 render/blit tags, object tags and display ops: what licence-clean sources say

Read-only research. No code was changed.

## Sources used

- MIT upstream `/tmp/ref/up` (MattJackson/libapplegfx-vulkan @7574eac, LICENSE verified MIT).
  - Files read: `src/protocol/opcodes.h`, `render_pass.h`, `object_resolver.h`.
  - `src/handlers/compute/{render_inner_ops,compute_inner_ops,blit_inner_ops,info_replies,exec_cmdbuf}.c`.
  - `src/dispatchers/*`, `src/handlers/display/display.c`, `src/handlers/sync/sync.c`.
  - No `docs/` file describes tags. The RE documents the code cites (`render-decoder-handlers.*`, `info-decoder-opcodes.md`, `inner-opcode-format.md`, `PROTOCOL.md`) are not in the repo. MIT upstream therefore cannot be checked against them.
- Our own device logs: 23 `g7_lagfx_*` uploads (12 distinct tag tallies), `/tmp/x231501` (R1) and `/tmp/x230109` (R2), and dump `index.txt` (`cap_draw` lines).
- Public Metal API knowledge (method semantics only), from memory; I did not re-fetch Apple docs.
- Not used for meaning: Henge, anyos/medfa12, pvg-vulkan, `/tmp/ref/r`, and `docs/arm64-*.md`.
- Our headers `arm64_draw_decode.h`, `arm64_exec_decode.h` and similar cite excluded sources. They were used only to read what our code does, never for meaning.

### Class legend

- **a1**: named, with a complete payload layout in MIT.
- **a2**: named, with a partial layout, or MIT files disagree with each other. The reason is in the notes.
- **b**: named only (name and payload size, or one field).
- **c**: not found in allowed sources.
- **d**: seen on our device, with a count.

MIT layouts are "parse-and-trace". Only 16 tags have ever been exercised against a live guest (see below), so "named" does not mean verified.

## Summary table

Counts are tags (or items) per census ID.

| ID | items | a1 | a2 | b | c | d (on device) |
|---|---|---|---|---|---|---|
| T05 draws 0,2,3,4,5,6,8,9,10,11,16,17 | 12 | 1 | 5 | 6 | 0 | 0 |
| T06 depth/raster 104,107,108,115,119 | 5 | 3 | 2 | 0 | 0 | 0 |
| T07 113,127,128,129 | 4 | 0 | 2 | 2 | 0 | 0 |
| T08 residency/fence 24,25,27,134,135,137 | 6 | 2 | 0 | 4 | 0 | 0 |
| T10 tile 155,156,158,159,160,163,164 | 7 | 4 | 2 | 1 | 0 | 0 |
| T12 the 26 listed tags | 26 | 4 | 7 | 15 | 0 | 0 |
| T13 28,29,37..100,138..152 | 81 | 8 | 4 | 7 | 62 | 0 |
| T13 167 and above | open | 0 | 0 | 0 | all | 0 |
| T14 blit 301..323 (23 tags) | 23 | 0 | 2 | 21 | 0 | 0 |
| O03 serialized tag 4 | 1 | 0 | 0 | 0 | 1 | 0 |
| O05 object tags (13, 0x36, 0x32, 0x0d, 7, 8, 0x1b, 19) | 8 | 0 | 0 | 0 | 8 | 0 |
| O05 info queries 0x1c2..0x1d5 | 20 | 0 | 15 | 0 | 5 | 0 |
| O08 texture-desc tags 0x34, 0x39, 0x14, 1 | 4 | 0 | 0 | 0 | 4 | 0 |
| K09 compute 0x34, 0x35, 0x3e, 0x3f | 4 | 0 | 0 | 2 | 2 | 2 (0x3e, 0x3f) |
| K13 compute 0x45 | 1 | 0 | 0 | 0 | 1 | 0 |
| K14 segment types 1, 4, other | 3 groups | 0 | 1 | 2 | 0 | 0 |
| R07 root 0x42 | 1 | 0 | 1 | 0 | 0 | 0 |
| D01, D02 (display ch4 0x01, 0x02) | 2 | 2 | 0 | 0 | 0 | 2 |
| ch4 0x0a, 0x0c | 2 | 0 | 0 | 0 | 2 | 2 |

Notes on the table:

- T13, tags 37..100: 64 tags, of which 62 are class c. The other two are 44 (0x2c, b) and 60 (0x3c, a2).
- T13, tags 28 and 29 are in class b, so the census claim "no meaning known" is wrong for them.
- K14: segment type 5 is the a2 entry. Types 1 and 4 are b, and MIT's numbering contradicts the device (see finding 2). Other numbers are c.

## Top findings

1. **All 16 tags seen on the device are named in MIT with matching payload sizes.**
   - Device tags (decimal): 1, 7, 26, 110, 111, 112, 114, 116, 117, 125, 126, 130, 157, 161, 162, and blit 300.
   - Tag number to MIT hex name to MIT payload size, against device size minus 8:

     | device tag | MIT name | MIT payload | device size - 8 |
     |---|---|---|---|
     | 1 | DrawPrimitives16 | 8 | 8 |
     | 7 | DrawIndexedPrimitives16 | 12 | 12 |
     | 26 | RenderDescribeRenderPass | 584 | 584 |
     | 110 | SetFragmentBuffers | 8 + N*12 | 20 (N=1) |
     | 111 | SetFragmentBufferOffset | 12 | 12 |
     | 112 | SetFragmentSamplerStates | 8 + N*4 | 12 (N=1) |
     | 114 | SetFragmentTextures | 8 + N*4 | 12 (N=1) |
     | 116 | SetRenderPipelineState | 4 | 4 |
     | 117 | SetScissorRect | 32 | 32 |
     | 125 | SetVertexBuffers | 8 + N*12 | 20 (N=1) |
     | 126 | SetVertexBufferOffset | 12 | 12 |
     | 130 | SetViewport | 48 | 48 |
     | 157 | SetTileBuffers | 8 + N*12 | 20 (N=1) |
     | 161 | SetTileTextures | 8 + N*4 | 12 (N=1) |
     | 162 | DispatchThreadsPerTileInRegion | 76 | 76 |
     | 300 | CopyFromBufferToTexture | 88 | 88 |

   - MIT's own live capture (macOS guest) shows the same set of render opcodes. So the numbering is shared between guests, and the whole table from 0x00 to 0xa6 is named.

2. **Segment-type numbering in MIT disagrees with the device.**
   - MIT comments say encType 0/1 is compute, 2 is render, 4 is blit/info, 5 is a protection preamble.
   - MIT's own observation table (`compute_inner_ops.c`) shows encType 0 carrying render opcodes (0x1a, 0x74, 0x7d and others, 181k events, 100% encType 0).
   - Device: type 0 carries render tags (316 in R1), type 2 carries blit tag 300. Types 1 and 4 and "other" never occur (`seg_t1=seg_t4=0`).
   - So MIT does not support our "type 1 = compute, type 4 = information" labelling.
   - Type 5 as a preamble (8 bytes of options, then a real segment header) is named in MIT. It is a2 and has not been seen on our device.

3. **MIT's own files disagree on several layouts.** Do not treat the "a" classes as verified.
   - Array head order for SetBuffers/Textures/SamplerStates:
     - `render_inner_ops.c` reads count@0 then first@4.
     - The live handlers in `compute_inner_ops.c` read firstIndex@0 then count@4, and say count-first was a mis-documentation.
     - The device agrees with first-first. Our code uses {index, count} and binds slots up to 9 from tags of 20 bytes of payload. Count-first would need 116 bytes for slot 9.
   - Compact draws:
     - 0x01 DrawPrimitives16: prim@0 (u16 or u32), start u16@4, count u16@6. The same bytes are decoded either way for prim=3.
     - 0x03: the live handler has {start, count, instances, prim}, each u16. The trace file has prim first.
     - 0x07: {prim u16, indexType u16, ref u32, count u16, offset u16}, 12 bytes. Our `cap_draw` lines are consistent: tag 7 gives prim=3, index_type=0, count up to 48, index_off up to 8592 (fits u16). Tag 1 gives prim=3, count up to 24.
   - Wide 0x06: two incompatible layouts in MIT (count first, or prim first). Neither was ever seen.
     - Size arithmetic from the compact 12-byte form favours {u16 prim, u16 type, u32 ref, u64 count, u64 offset} = 24. That is my inference only.
   - 0x3c: the table says 20 bytes, the handler reads 8.

4. **Never-seen tags are named but weakly specified.**
   - MIT assigns names to 0..29 (except 14's omission from the census, below) and 101..166. Layouts for those tags are trace-level only.
   - Tags 30..36 (0x1e..0x24) are only mentioned in a `render_pass.h` comment as the pass sub-stream (class c). T11 lists them.
   - Tag 14 (DrawIndexedPatches64, 60 bytes) is missing from every census ID. It is class b.
   - Tags 37..100 other than 44 and 60, and 167 and above, are absent (c). The MIT table ends at 0xa6 = 166.
   - Several MIT names have no public Metal counterpart: SetAlphaTestReferenceValue, SetPointSize, SetClipPlane, SetProvokingVertexMode, SetTransformFeedbackState, SetDepthCleared, SetStencilCleared, FillBufferWithPattern, FillTextureWithBytes, FillTextureWithColor, InvalidateCompressedTexture*. Their meaning rests on MIT names alone.

5. **Object, descriptor and texture tags are not in MIT.**
   - Not found: serialized tags 4, 3, 0x33; object tags 13, 0x36, 0x32, 0x0d, 7, 8, 0x1b, 19; texture-description tags 0x2f, 0x34, 0x39, 0x14, 1.
   - MIT only knows slot types 6 (function) and 7 (pipeline), plus the pipeline token grammar `04 <u32> <tag>` for vertex attributes and stride.
   - Info queries 0x1c2..0x1d0 are named with header layout {u32 ref, u32 buffer_id, u64 reply_offset} and reply sizes. 0x1d1..0x1d5 are absent.
   - Root 0x80, 0x81, 0x82 (heap texture size/align, reset rasterization rate, delete shared tex back) are named but log-only.

6. **Corrections to the census.**
   - D02 payload is {u32 0, u32 1}, not {u32 1, ...}.
   - Ch4 0x0c payload word 4 is 0x4000, not 0x40.
   - 0x3e/0x3f entries are 4 bytes (u32 refs), not {ref, flags} pairs. This matches what our code does for 0x35 and 0x3e.

## Per-ID notes

### T05 draws

MIT names (hex = tag), payload size, and the fields it parses:

- 0 DrawPrimitives64, 20: prim u32@0, start u64@4, count u64@12 (a1).
- 2 DrawInstancedPrimitives64, 28: prim u32@0, start u64@4, count u64@12, instances u64@20 (a2, conflicts with compact 0x03 order).
- 3 DrawInstancedPrimitives16, 8: see finding 3 (a2).
- 4 DrawInstancedBasePrimitives64, 36: prim@0 only (b).
- 5 DrawInstancedBasePrimitives16, 12: prim@0 only (b).
- 6 DrawIndexedPrimitives64, 24: conflicting layouts (a2).
- 8 DrawIndexedInstancedPrimitives64, 32: type@0, count@4 (a2).
- 9 DrawIndexedInstancedPrimitives16, 16: type@0 (b).
- 10 DrawIndexedInstancedBasePrimitives64, 48: type@0 (b).
- 11 DrawIndexedInstancedBasePrimitives16, 20: type@0 (b).
- 16 DrawPrimitivesIndirect, 16: type@0, arg_buf_ref@4 (a2).
- 17 DrawIndexedPrimitivesIndirect, 28: type@0 (b).

Public Metal gives the semantics of the corresponding `drawPrimitives:...`, `drawIndexedPrimitives:...`, `...Indirect...` family. It does not give wire order.

Size arithmetic: compact to wide goes 8 to 20, 8 to 28, 12 to 36, 12 to 24, 16 to 32, 20 to 48. The wide prim field is probably u32 where the arithmetic is exact (28 = 3*8+4, 36 = 4*8+4). This is my inference only.

### T06

- 104 SetDepthStencilState {u32 object ref} (4 bytes, a1).
- 108 SetDepthBias, 3 x f32 {bias, slopeScale, clamp}. Matches public `setDepthBias:slopeScale:clamp:` (a1).
- 119 SetStencilRef {u32 front, u32 back} (a1).
- 107 SetCullMode and 115 SetFrontFacingWinding are 8 bytes: {u32 value, u32 extra}. The second word is unexplained (a2).
- On device these tags never occur. `cap_draw` shows cull=0, winding=0, bias=0,0,0 on all 256 captured draws, so those are defaults.

### T07, T08, T10

- 127 SetVertexSamplerStates and 129 SetVertexTextures use the same array shape as 112 and 114, which the device confirms (a2 by analogy).
- 113 and 128 (...LODClamp): MIT gives entry size 12 only. Public `lodMinClamps` / `lodMaxClamps` are floats, so {ref u32, lodMin f32, lodMax f32} is plausible (my inference, b).
- 24 RenderUpdateFence {fence_ref, stages}, 25 RenderWaitForFence {fence_ref, stages}, 8 bytes each (a1).
- 27 UseHeapsWithStages, 134 UseHeaps, 135 UseResources, 137 UseResourcesWithStages: only header words are known ({count, usage, stages}). Reference entries are unspecified (b).
- T10:
  - 155 DispatchThreadsPerTile: 3 x u64 (24 bytes).
  - 156 SetRenderThreadgroupMemoryLength: {len u64, offset u64, index u32}.
  - 158 SetTileBufferOffset: {index u32, offset u64}.
  - 164 GetTileDimensions: {ref u32, offset u64}.
  - 159 SetTileSamplerStates: same array shape as 112.
  - 160 ...LODClamp: entry 12.
  - 163 DispatchThreadsPerTileInRegionWithIndex, 76 bytes: only tile u64 x 3 @0 and rtIdx u32@72 are named. 162 at 76 bytes occurs on device 28 times.

### T12 and T13

- The T12 tags are all named. a1: 101 SetBlendColor (4 x f32), 123 TessFactorScale (f32), 136 SetLineWidth (f32), 166 SetVertexBufferOffsetWithStride {index u32, offset u64, stride u64}.
- Names for the rest of T12:
  - Draw/ICB: 12 DrawPatches64, 13 DrawPatches16, 15 DrawIndexedPatches16, 18 DrawPatchesIndirect, 19 DrawIndexedPatchesIndirect, 20 ExecuteCommandsInBuffer (icb_ref@0, range_buf_ref@4), 21 ExecuteCommandsInBufferRanged (icb_ref@0).
  - State: 102 and 103 {value, index}, 105/106/120/121 store actions and options, 109 DepthClipMode, 118 ScissorRects, 122 TessFactorBuffer, 124 TriangleFillMode, 131 Viewports, 132 VisibilityResultMode {mode u32@0, offset u64@8}, 153 VertexAmplificationMode, 154 VertexAmplificationCount, 165 VertexBuffersWithStride (entry 20).
- T13 named tags:
  - 28, 29: DrawIndexedInstancedBasePrimitives64_2 (48) and ...16_2 (20).
  - 44 (0x2c): "Unknown", 88 bytes, hex-dumped by MIT.
  - 60 (0x3c): command-buffer-inner {u32 resource_id, u32 pad}.
  - 138..152: AlphaTest, PointSize, ClipPlane {idx, 4 x f32}, VertexSamplerState and FragmentSamplerState {ref@0, idx@16}, ViewportTransformEnabled, ProvokingVertexMode, PrimitiveRestartIndexEnabled {enabled, index}, TriangleFillModeFrontBack, TransformFeedbackState, DepthCleared, StencilCleared, ColorResolveTexture {ref, slot}, DepthResolveTexture, StencilResolveTexture.

### T14 blit

MIT names for 300..323 (hex 0x12c..0x143), with payload sizes:

| tag | name | size |
|---|---|---|
| 300 | CopyFromBufferToTexture | 88 |
| 301 | CopyFromBufferToBuffer | 32 |
| 302 | CopyFromTextureToBuffer | 88 |
| 303 | CopyFromTextureToTexture | 88 |
| 304 | CopyFromTextureToTextureWithOptions | 92 |
| 305 | CopyIndirectCommandBuffer | 32 |
| 306 | FillBuffer | 24 |
| 307 | GenerateMipmaps | 4 |
| 308 | OptimizeForCPUAccess | 4 |
| 309 | OptimizeForGPUAccess | 4 |
| 310 | OptimizeImageForCPUAccess | 8 |
| 311 | OptimizeImageForGPUAccess | 8 |
| 312 | OptimizeIndirectCommandBuffer | 20 |
| 313 | ResetCommandsInCommandBuffer | 20 |
| 314 | SynchronizeResource | 4 |
| 315 | SynchronizeTextureImage | 8 |
| 316 | BlitUpdateFence | 4 |
| 317 | BlitWaitForFence | 4 |
| 318 | CopyFromTextureToTextureWithNumSliceLevel | 20 |
| 319 | FillBufferWithPattern | 24 |
| 320 | FillTextureWithBytes | 76 |
| 321 | FillTextureWithColor | 92 |
| 322 | InvalidateCompressedTexture | 4 |
| 323 | InvalidateCompressedTextureImage | 8 |

- Field layouts exist only for 303 and 321. MIT itself says they are "pre-refactor" and the live wire "may interleave a u32 pad". Treat them as unverified (a2).
- 300 occurs on device 780 times over 12 logs, size 96 (88 + 8), which matches MIT's 88.
- MIT's blit table is ack-only for everything else.

### O03, O05, O08

All class c, except the info queries (a2). See finding 5.

### K09

- MIT names 0x34 and 0x35 as "ChannelEventMachine-adjacent" kext pings (log and ack). That conflicts with our "resource sync/invalidate" use. There is no device occurrence to decide between them.
- 0x3e and 0x3f are absent from MIT. MIT's root namespace has 0x06 invalidate, 0x07 discard, 0x23 synchronize-discard and 0x42 synchronize, which are different numbers (class c here).
- Device (`diag cmd` raw bytes): 0x3e and 0x3f occur on ch3 as {u32 task=2, u32 count, u32 refs[count]}. Examples:
  - 0x3e len 24: `02 00 00 00 | 01 00 00 00 | c6 00 00 00`.
  - 0x3e len 28: task 2, count 2, refs 0x89, 0x71.
  - 0x3f len 24: refs 0x4d.
  - 0x3f len 28: refs 0xcb, 0xce.
  - Both opcodes: arg8 = 0.
  - Raw lines logged: 0x3e 31 + 21, 0x3f 15 + 48. This is a subset: only the first N commands per log are dumped.

### R07 root 0x42

- MIT: {u32 task_id, u32 range_count, ranges}. Its comment says "{u64 gpa, u64 length}" records but the size check uses 8 bytes per range, so it contradicts itself (a2). The handler returns NOT_IMPLEMENTED.
- Device: 0 occurrences.

### K10: could the ARM64 executor accept a 0x37 command? (layout compatibility only)

- MIT dispatcher (`channel_compute_dispatcher.c`): 0x37 is routed to `lagfx_compute_exec_cmdbuf`. The comment says the outer layout is 12 + dc*24 + rc*16.
  - MIT handler: {u32 task, u32 descriptor_count, u32 resource_count} at payload+0, +4, +8.
  - Then dc records of 24 bytes (opaque; only hexdumped).
  - Then rc records of 16 bytes {u64 host_gpu_addr, u32 length, u32 pad} (resource table).
  - A payload of 8 bytes is a "minimal query" ({task} plus one word).
- Our `lagfx_arm64_exec_decode` (0x43 and 0x45):
  - Same 12-byte head {task, nres, nbuf}, after `arg_count_8b*8` barrier bytes.
  - Then nres records of 8 bytes {ref, flags}.
  - Then nbuf records of 16 bytes {u64 off, u64 len}.
- Compatibility:
  - Head: identical size and field positions.
  - 16-byte records: same size and the same first u64. Our second u64 is "len"; MIT's is u32 length plus u32 pad. Compatible only if the upper 32 bits are zero.
  - Middle block: MIT has 24-byte records and ours has 8-byte records, so a MIT-shaped 0x37 would fail our size check `12 + 8*nres + 16*nbuf <= payload` for any non-zero count. It would fail as BAD-SIZE, or be misparsed if the counts happen to fit.
  - A device 0x43 (len 88) decodes as head {2, 6, 1}, then 6 x {ref, flags}, then 1 x 16 bytes. That is our layout, not MIT's.
  - 0x37 has 0 occurrences in all logs. In the arm64 path our dispatcher routes it to the x86 walker, and I did not examine that behaviour.
  - Conclusion: the layouts are not compatible beyond the head. MIT's layout looks like an x86-guest layout. No arm64 evidence exists for 0x37.

### Compute 219 (0xdb)

- MIT: no occurrence of 0xdb or 219 as an opcode, tag, or constant anywhere in the repo.
- Device logs: 0 commands with op 0xdb on any channel. The only 0xdb-containing values are stamps (`stamp=0x0000db00`, 70 lines, which is sequence 0xdb << 8), ring pointers (rp/wp 0xdb0 and similar), and a pool address.
- It also appears as the low byte of the D01 payload word 0x000200db ({display_index 0, ss_pfn 0x200db} if MIT's 0x01 layout holds). That is a coincidence, not an opcode.
- The only document in our tree that names 0xdb (as "writeDescriptor") is the excluded `docs/arm64-opcode-comparison.md`. External, unverified, not usable for the MIT library.
- Class c, with no device occurrence.

### Display ch4 0x0a / 0x0c

Header is 12 bytes {u16 op, u16 arg8, u32 len, u32 stamp}. arg8 = 0 for both ("barrier bytes 0"). Observed on `src=compute ch=4`, 16 raw lines each across the logs (1 per run for 0x0c, 2 per run for 0x0a in R1).

Neither opcode is named in MIT for a compute-channel namespace. MIT's 0x0a (GetDeviceInfo) and 0x0c (Delay) are root-channel opcodes in a disjoint namespace, with different payloads. Class c.

**0x0c, len 36, stamp 0x300, payload 24 bytes (fully visible):**

`00000000 18020000 00401100 00000000 00400000 00000000`

- u32 words: 0x0, 0x218, 0x114000, 0x0, 0x4000, 0x0.
- Alternative reading as 3 x u64 (little endian): 0x0000021800000000, 0x114000, 0x4000.
- Observations only, no meaning assigned:
  - 0x114000 equals the `off` of the buffer in the 0x43 exec on the same task (b0 off=0x114000, seen in the same logs).
  - 0x4000 equals the ring size and 16 KiB page size seen in the same logs.
  - 0x218 is a plain integer, not obviously a pointer.
- Identical bytes in every run.

**0x0a, len 76, stamps 0x500, 0x1000, 0x2300, 0x2500 and others, payload 64 bytes, first 52 visible (the log truncates the raw dump at 64 bytes total):**

`00000000 01000000 00000000 0c000000 0000803f 00000000 00000000 00000000 00000000 0000803f 00000000 00000000 00000000`

- u32 words: 0x0, 0x1, 0x0, 0xc, 1.0f, 0, 0, 0, 0, 1.0f, 0, 0, 0. The last 12 bytes (3 words) are not logged anywhere.
- Words 4 and 9 are float 1.0, five words apart, with zeros between. That is consistent with a diagonal pattern of a 4x4 float matrix, but only if word 14 is also 1.0. I cannot confirm this from the logged bytes.
- Word 3 is 0xc (12), word 1 is 1. They look like small integers, ids or counts. No field looks like a pointer or a size.
- Identical bytes in every run. A run with a longer raw dump is needed to see the tail.

**Other ch4 ops seen, for comparison (raw bytes, payload 8 bytes each):**

- 0x01 {0, 0x200db}. MIT names compact display opcode 0x01 "setupSharedState" with payload {u32 display_index, u32 ss_pfn}. The shapes match; MIT places it on ch 5+ in x86 guests, and ours arrives on ch4.
- 0x02 {0, 1}. MIT names compact 0x02 "displaySubmit" with payload {u32 display_index, u32 arg2}.
- 0x09 {0, 1}, seen 2 times in old logs. MIT's 0x09 is a root-only "replace physical". Class c here.
- MIT's compact display ops also include 0x04, 0x06 (present, 12 bytes) and 0x07 (present+gamma, 36 bytes). 0x08, 0x0a and 0x0c have no MIT name.

## Namespace, length, alignment per tag (as seen in our dumps)

**Outer ring commands (all channels).**
- Header: 12 bytes {u16 opcode, u16 arg8, u32 total length incl. header, u32 stamp}. Payload follows immediately. `arg8*8` barrier bytes sit at the start of payload on commands that use them (0 in every log).
- Command lengths are multiples of 4 only, for example 20, 36, 52, 76, 88. Payload structs are packed with no padding.
- Namespaces:
  - Root (ch0): 0x30, 0x33, 0x38, 0x3a.
  - Compute (ch1..4): 0x22, 0x25, 0x43, 0x44.
  - Ch3 carries 0x3e and 0x3f.
  - Display-like commands on ch4: 0x01, 0x02, 0x08, 0x09, 0x0a, 0x0c.
  - Opcode values overlap between namespaces (0x0a, 0x0c, 0x38, 0x25), so a number only has meaning together with its channel.

**Inner tags inside an exec segment (0x43 payload).**
- Buffers start with {u32 seglen, u32 info}; info byte 0 is the segment type.
- Commands inside are {u32 tag, u32 size incl. this 8-byte header}.
- Sizes seen: 12 (116), 20 (111, 126, 112, 114, 161, 7), 16 (1), 28 (110, 125, 157), 40 (117), 56 (130), 84 (162), 592 (26), 96 (300 in a type-2 segment).
- All sizes are multiples of 4 and only tag 26 (and the 8-multiples 16, 40, 56, 96) are multiples of 8. Commands are therefore 4-byte aligned, and fields such as u64 at payload+4 are unaligned.
- Namespaces: render tags 0..166 in type-0 segments, blit tags 300..323 in type-2 segments.
- The render and blit tag spaces do not overlap (render below 167, blit from 300).
- Info queries 0x1c2.. (type 4) and the objects below are separate namespaces.

**Object descriptors (guest memory, referenced by ref).**
- Texture description for a mapped surface: tag 0x2f, size 48 bytes, words `01000000 00000000 2f000000 30000000 ...`, so {u32 1, u32 0, u32 tag, u32 size, ...}, all u32 aligned.
- Serialized objects (type 7): tag 0xe (PSO, 336 and 368 bytes, header `0e000000 <size> ...`) and tag 3 (sampler, 36 bytes).
- Both are 4-byte aligned. Nothing in MIT names these tags.

## Memory facts

Nothing new about the user worth saving came from this work.
