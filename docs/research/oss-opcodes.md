# PVG guest command-stream opcodes: what public OSS says

Research date 2026-11-27. Method: shallow clones of public repos into /tmp/oss and /tmp/lagfx-clone, then reading code and comments. Facts are paraphrased. Fetched content was treated as data. No repo changes were made.

## 0. Numbering and namespaces (read first)

Three numbering conventions are in use. They are not conflicts.

- **Outer channel/FIFO command** (header: u16 opcode, u16 barrier-count, u32 size, u32 stamp; payload starts at 12 + 8*barriers). Examples: 0x37, 0x43, 0x45, 0x44.
- **Inner serialized command stream** (records `{u32 opcode, u32 total_len}` + payload). Render uses 0x00-0xa6 plus 0x1a, 0x2c, 0x3c. Compute uses 0xc8-0xe6. Blit uses 0x12c-0x141. Info uses 0x1c2-0x1d5.
- **Object-list type tag** (low byte of the first word of each 12-byte object-list entry). Examples: 1, 2, 14, 19.

Henge writes inner opcodes in decimal. The libapplegfx-vulkan and pvg-vulkan wire crates write the same opcodes in hex.

| Your number | Hex | Namespace |
|---|---|---|
| 165/166 | 0xa5/0xa6 | render inner |
| 209/211/216/219 | 0xd1/0xd3/0xd8/0xdb | compute inner |
| 154 | 0x9a | render inner |
| 12-15, 18, 19 | 0x0c-0x0f, 0x12, 0x13 | render inner |
| 307-315 | 0x133-0x13b | blit inner |
| 450-series | 0x1c2... | Info inner |

0x37 also appears as an inner opcode with a different meaning. In pvg-vulkan's wire crate it is a wide buffer-backed-texture create record. In Henge it is a texture descriptor v2 object tag. This is a different namespace from outer 0x37.

## 1. Sources

All of the sources below were fetched and read directly at the commit shown.

| Source | Commit | Licence | Evidence type |
|---|---|---|---|
| libapplegfx-vulkan (MattJackson, "upstream") | 7574eac0d7396a9f49366f007c7fbeb44245826c (2026-09-06) | MIT (LICENSE, "Copyright (c) 2026 Matthew Jackson") | Host-side decoder. Claims are based on kext disassembly and live traces, but those notes are not published. |
| Leviidev/Henge | 7b557301195f130f900b1b4cd105c9ecf0fbc109 (2026-11-15) | GPL-2.0-or-later (docs/licensing.md). Facts only. | Replay decoder for guest traffic on real guests (macOS 13/14/15+/27 mentioned). Code comments say "formats worked out from the guest driver". |
| medfa12/apple-silicon-anyos-project, vendored `vendor/pvg-vulkan` | 59fd73aac4ae386939bc69dc58dffbce1b2167b2 (2026-08-17) | Conflicting. Root LICENSE is GPLv2. README says the PVG device is LGPL-3.0-or-later. vendor/pvg-vulkan/LICENSE is LGPL-3 text. Cargo.toml says GPL-2.0-or-later. Treat as copyleft; facts only. | Best evidence. A "wire oracle" drives Apple's own PGSerializer on macOS 26.5 (AppleParavirtGPUMetal 64.4.7) with sentinel arguments and records the bytes. Other facts come from live-boot logs. |
| steelbrain/reims-vgpu (f83b1ed0, 2026-11-26) and steelbrain/metal2vulkan (34fcb1ef, 2026-11-26) | as shown | GPL-2.0-or-later OR LGPL-3.0-or-later (reims-vgpu) / dual (metal2vulkan) | **No PVG opcode content.** reims-vgpu uses its own RGFX wire protocol and its own PCI ID. Its design.md says the Apple paravirt lane was deleted on 2026-11-07. metal2vulkan is a Metal-to-Vulkan runtime. grep found no CmdExec/ExecIndirect/ChannelEvent. Its only relevance is that it measured Metal behaviour (imageblock size limits, vertex amplification count 2 on M3 Max). |
| QEMU hw/display/apple-gfx.m | n/a | GPLv2+ | Not read in depth. It hands the stream to Apple's ParavirtualizedGraphics.framework and contains no opcode knowledge. Unverified beyond search-result snippets. |
| mos-qemu, mos-docker (MattJackson) | not examined for opcodes beyond grep | GPLv2 / MIT | The libapplegfx README says the narrative docs live in mos-docs and a `paravirt-re` library. Both are private and returned an auth error. The full opcode catalogue those code comments cite (PROTOCOL.md §13, whitepaper 02) is therefore not publicly checkable. |
| Blogs, gists, other write-ups | web search only | n/a | The search found only QEMU patches, an Apple forum thread and Asahi blogs. Nothing public enumerates PVG opcodes beyond the repos above. |

Caution: one WebFetch summary of the libapplegfx repo described files that do not exist in the clone, such as `src/protocol/opcodes.c` and `compute_opcodes.c`. Only claims confirmed in the clone are used below.

## 2. Findings per item

Evidence labels: LIVE-PAYLOAD means bytes captured from real guest traffic. ORACLE means bytes written by Apple's serializer under test with sentinel values. KEXT-RE means disassembly. INFER means guessed.

### 2.1 Outer opcode 0x37 (CHANNEL_EVENT_37 upstream)

**Conclusion: the best-supported reading is `CmdExecIndirect2`, a per-channel exec of command buffers. "Channel event" is a stale name.** Confidence is high, because three independent projects agree.

- libapplegfx, `src/dispatchers/channel_compute_dispatcher.c` lines ~86-98 at 7574eac.
  - Claim: the earlier kext-disasm pass classified 0x37 as "ChannelEventMachine-adjacent". A later M4 RE pass found it is the per-channel variant of CmdExecIndirect2 (0x20).
  - The outer payload layout is 12 + dc*24 + rc*16. It is routed to the exec handler (`exec_cmdbuf`), which handles empty (event-only) and populated payloads.
  - The opcodes.h enum name `CHANNEL_EVENT_37` was never renamed. Line 107 still says "ChannelEventMachine-adjacent".
  - `channel_0_dispatcher.c` ~103 repeats "0x37 on ch 1..4".
  - `upstream-pr/src/protocol/opcodes.c` ~152-170 shows the earlier P2 state: payload shape unknown, name "ChannelEvent37".
  - Evidence: KEXT-RE plus live observation (the 0x28 comment says "Live evidence"). `tests/cmdexecindirect2-parser.c` uses opcode 0x37.
- Henge `devices/pvg_metal.m` ~2768-2781.
  - Claim: 0x37 is "macOS 13's driver" form of the same exec as 0x43.
  - Layout: {task, nres, nbuf}, then 24-byte resource entries (the first two words are {ref, flags}, plus four more words), then 16-byte command-buffer entries.
  - Henge converts it to the 0x43 shape (8-byte {ref, flags} entries).
  - The size check is 12 + nres*24 + nbuf*16 <= n. Evidence: LIVE-PAYLOAD, on a macOS 13 guest.
- pvg-vulkan `crates/pvg-vulkan/src/model/regs.rs` line 512, `CHILD_OP_EXEC_INDIRECT2 = 0x37`. `runtime/decode/fifo.rs` lines ~107-125 give the layout.
  - Header: {u32 task, u32 resource_count, u32 cmdbuf_count}, 12 bytes.
  - Resource record: 24 bytes. {u32 object_id, 4 validity-op bytes, 16 bytes zeroed by the queue}.
  - Command-buffer descriptor: 16 bytes. {u64 gva, u32 length at +8, and a pad}.
  - Evidence: live boot logs plus an Apple host dispatch table (the "reference host").
  - `regs.rs` line ~372 (`CHILD_OP_MAX = 0x46`) says Monterey's host bounds opcodes at <= 0x40 and Tahoe extends the table to 0x46.
- Treatment: all three projects execute the command buffers. None ignores 0x37.
- Naming caveat: libapplegfx's own comment says "kext-side per-channel exec variant" (0x37) versus "dylib emits 0x20 on root channel". pvg-vulkan's CHILD_OP_DELETE_TASK is also 0x20, so 0x20 is overloaded between a root-channel exec and DeleteTask. Henge (`pvg_metal.m` ~2778) treats 0x20 as task delete (`args[0]>>1`). This is a possible conflict with libapplegfx's "0x20 = ExecIndirect2". Unresolved.

### 2.2 Outer opcodes 0x43 and 0x45

**Conclusion: both are newer (Tahoe / macOS 26+ era) compact exec forms. Same {task, resource count, cmdbuf count} header and 16-byte command-buffer descriptors as 0x37, but 8-byte resource records.**

- pvg-vulkan `regs.rs` lines ~548-556.
  - 0x43 `EXEC_INDIRECT3`: Tahoe's compact submission form. 12-byte header, 16-byte cmdbuf descriptors, 8-byte resource records. `fifo.rs` ~111-113 gives the record as {object_id, residency_usage} with usage at +4. `fifo.rs` ~393-431 expands it to the canonical 24-byte table.
  - 0x45 `EXEC_INDIRECT4`: "later compact" form. Live payloads have the same shape as 0x43.
  - 0x44 is `CmdSetObjAndPlace`, `setResourceHeap`, seen right after each Tahoe DefineTask2.
  - Evidence: LIVE-PAYLOAD from macOS 26 guests.
- Henge `pvg_metal.m` ~2766: `if ((op == 0x43 || op == 0x45) && n >= 12)` calls the same exec routine. Henge `pvg_lite.c` ~360 and ~542 say that from macOS 15 each task's object list is named by 0x44. Older guests, macOS 14 seen, never send it, and their list sits at task offset 0x4000. Evidence: LIVE-PAYLOAD.
- libapplegfx at 7574eac does not define 0x43 or 0x45. Its opcodes.h stops at 0x41 (`EXEC_INDIRECT_EXT_41`). The "0x00..0x44 tolerated" comment says the extras are stubbed as unknown. That is a gap relative to Tahoe guests. Confidence: high (grep of the full clone).
- Why 0x43 and 0x45 differ: not established. pvg-vulkan and Henge both treat them identically. Whether 0x45 carries extra semantics, such as a different usage-bit meaning, is unverified.

### 2.3 Compute inner opcode 219 (0xdb)

**Conclusion: it is `ComputeCommandEncoder writeDescriptor`, the compute pass descriptor. The payload is one u32 dispatch type (0 = serial, 1 = concurrent, per the oracle). Total length is 12 bytes.** Confidence: high.

- pvg-vulkan-wire `src/ops/compute.rs` lines ~624-655. Manifest entry at `manifest.rs` ~1353.
  - `setCurrentDispatchType:` writes nothing. The type reaches the wire only via this record when the guest writes the descriptor.
  - Fixtures: the encoder's starting type reads 0, and after setting 1 it reads 1.
  - Evidence: ORACLE on macOS 26.5.
- Henge `pvg_metal.m` ~1900: `case 219` is "not a command the framework knows either, though the guest sends it". It is a no-op.
  - It sits next to the `default:` handler that logs "compute command N skipped", so ignoring it is deliberate.
  - Ignoring it loses the dispatch type. Metal fixes a compute encoder's dispatch type when the encoder is created, so a host that creates the encoder from the encType (serial or concurrent) would need to read this record. It is therefore a possible correctness gap. INFER.
  - "Reported as unknown/ignored in some implementations" is confirmed for Henge. libapplegfx has no compute table covering 0xc8-0xe6 at all (see section 3).
- pvg-vulkan fixture-backed layout for neighbours: 0xdc-0xe2 are GPU control flow (`setSupportsCommandBufferJump`-gated), 0xe3 is the compressed-texture flush, 0xe4/0xe5 are ICB execute, and 0xe6 is dispatchThreadsIndirect.

### 2.4 Compute 209, 211, 216

All three are high confidence and have independent agreement.

- **209 (0xd1) setStageInRegion.** Total length is 56 bytes (6 x u64).
  - pvg-vulkan `compute.rs` ~352-382. ORACLE. The record stores **size first, then origin**: width, height, depth, x, y, z. This is the reverse of `MTLRegion`, and the doc comment says so explicitly. A fixture distinguishes the six values.
  - The sibling 0xd2 is the indirect form: {u32 buffer ref, u64 offset}, 20 bytes. Ref leads and offset trails, the reverse of the dispatch-indirect record.
  - Henge `pvg_metal.m` ~1801-1807: `memcpy`s the record straight into an `MTLRegion` and calls `setStageInRegion:`. **Possible bug in Henge. If the record is size-first, the origin and size are swapped.** This is INFER, because Henge's trace evidence for this record is unknown and the oracle is from macOS 26.5.
- **211 (0xd3) setThreadgroupMemoryLength:atIndex:.** {u64 length at +0, u32 index at +8}, total 20.
  - pvg-vulkan `compute.rs` ~411-435, ORACLE (length 0x1100 at index 3).
  - Henge ~1886 reads u64 at 0 and u32 at 8. Agrees. LIVE.
- **216 (0xd8) setImageblockWidth:height:.** {u32 width, u32 height}, total 16.
  - pvg-vulkan `compute.rs` ~595-622, ORACLE. Gated on `-setSupportsImageBlocks:`.
  - Henge ~1896: calls `setImageblockWidth:height:` only if both are non-zero and <= 256.
  - Henge comment: macOS 27's glass and blur kernels use a 32x32 imageblock. Without the call, toolbars rendered black with coloured blocks. This is LIVE evidence of impact.
  - reims-vgpu `docs/accuracy-ledger.md` ~123 (line anchor approximate) adds that Metal's validation layer refuses non-power-of-two sizes and sizes above 1024 cells, measured on an M3 Max.
- libapplegfx: the string "imageblock" appears only in Info replies 0x1ca/0x1cb (imageblock memory length, reply fixed to 0), not as encoder commands.

### 2.5 Render 165 and 166 (0xa5/0xa6) vertex buffers with attribute stride

- pvg-vulkan `ops/render.rs` ~913-1000, ORACLE.
  - 0xa5 is `setVertexBuffer(s):…attributeStride:…` and `setVertexBytes:length:attributeStride:atIndex:` (staging buffer ref).
  - Payload: 8-byte bind header {first index, count}, then per entry 20 bytes {u32 buffer ref, u64 offset, u64 stride}.
  - 0xa6 is `setVertexBufferOffset:attributeStride:atIndex:`: {u32 index, u64 offset, u64 stride}, total 28.
  - The records are only emitted when the serializer's `-supportsDynamicAttributeStride` capability is on (default false). A decoder that knows only 0x7d does not see the bind.
- Henge `pvg_metal.m` ~1611-1626 (cases 165 and 166). Same layout. Binds via `setVertexBuffers:...attributeStrides:`. LIVE.
- libapplegfx `render_inner_ops.c` ~1041-1042. Names 0xa5 `SetVertexBuffersWithStride` (variable) and 0xa6 `SetVertexBufferOffsetWithStride` (20-byte payload = 28 minus the 8-byte header). The handlers log only. Consistent. Evidence: RE table.

### 2.6 Render 154 (0x9a)

**It is `setVertexAmplificationCount:viewMappings:`. It is not an independent "multiple viewports" command.** The payload is `{u32 count, then count x {u32 viewportArrayIndexOffset, u32 renderTargetArrayIndexOffset}}`.

- pvg-vulkan `ops/render.rs` ~1005-1072, ORACLE. Fixture: 2 mappings in a 28-byte record. There is no `first` field. Reading it with the 8-byte BindHeader makes the first viewport offset look like the count. 0x99 is `setVertexAmplificationMode:value:`, with two u32s, 16 bytes.
- Henge ~1755-1765. Reads count (limit 8) and applies it with `setVertexAmplificationCount:viewMappings:` after `supportsVertexAmplificationCount:`. LIVE.
- libapplegfx ~1030, `SetVertexAmplificationCount`, log-only handler, size 0. SetViewports (multi) is a separate opcode, 0x83.
- The "multiple viewports/render-target offset" wording you saw is the per-view mapping inside this record. It does not refer to 0x83.

### 2.7 Render tessellation draws 12, 13, 14, 15, 18, 19

**The three projects disagree on opcode 0x0e. No public source settles it for macOS 27 guests.**

- pvg-vulkan `ops/render.rs` ~1630-1740, ORACLE (macOS 26.5):
  - 0x0d = compact `drawPatches` (24 bytes total). 0x0f = compact `drawIndexedPatches` (32 total).
  - **0x0c = BOTH wide forms.** Length is the discriminator: 56 bytes for plain patches, 68 bytes for indexed patches. 0x0e is simply unused.
  - 0x12 = indirect patches (36 total), 0x13 = indirect indexed patches (48 total).
  - Every field lists `control_points` last as u16.
  - The manifest records this as {0x0f, 0x0c} for the indexed selector.
- Henge ~1633-1676 (LIVE, but only for the cases it saw):
  - 13 = compact plain.
  - 12 = wide plain, with `control points` as a 16-bit field at +44.
  - 15 = compact indexed. 14 = wide indexed (u64 start/count, patch ref, control-point ref, control_points at +56).
  - 18 and 19 = indirect and indirect-indexed. Layout matches pvg-vulkan.
  - All of them are skipped when no tessellation factor buffer is set, because "Metal crashes without a factor buffer".
- libapplegfx `render_inner_ops.c` ~953-960: 0x0c `DrawPatches64` (48-byte payload), 0x0d `DrawPatches16` (16), 0x0e `DrawIndexedPatches64` (60), 0x0f `DrawIndexedPatches16` (24), 0x12 (28), 0x13 (40). Handlers are log-only. The payload sizes equal the oracle totals minus 8, apart from the 0x0e opcode.
- Discrepancy (flagged). A wide indexed draw arriving on 0x0c, as the oracle says, would hit Henge's `case 12` and be decoded as a plain draw, shifting every field after the ref. Henge's `case 14` would then never fire. INFER. Confirming it needs a trace of a guest with a wide indexed patch draw. The serializer (oracle) and the host decoder (libapplegfx, Henge) can legitimately differ: a host may accept 0x0e even though the serializer never emits it.

### 2.8 Blit 307-315 (0x133-0x13b)

- pvg-vulkan `ops/blit.rs` lines 65-68, 117-119, 167-168, ORACLE:
  - 0x133 generateMipmaps. 0x134/0x135 optimizeContentsForCPU/GPUAccess.
  - 0x136/0x137 the slice/level forms. 0x138 optimizeIndirectCommandBuffer. 0x139 resetCommandsInCommandBuffer.
  - 0x13a synchronizeResource. 0x13b synchronizeTexture(slice, level).
  - The `optimize:withCommand:` family writes the caller's value into the opcode field, so no fixed opcode exists for it.
- libapplegfx `blit_inner_ops.c` ~236-245. Same names, in the same order. 0x138/0x139 have a 20-byte payload and 0x133-0x135 have 4. Everything except `CopyFromTextureToTexture` and `FillTextureWithColor` is an ack stub.
- Henge `pvg_metal.m` ~1963-1971: only 307 does work (`generateMipmapsForTexture:`). 308-311, 314 and 315 are no-ops ("hints, and syncing memory Apple GPUs share"). 312 and 313 (ICB optimize/reset) fall to the "skipped" default.
- Confidence: high on the names and meanings. Being a no-op on a host is a design choice and has no basis in capture.

### 2.9 Object types 14 and 19

- **14 = heap buffer.** Henge `pvg_metal.m` ~1152 and `makeHeapBuffer` ~643-655.
  - The entry's pointer references a record {u32 heap ref, u64 offset, u64 length}, packed.
  - The host creates `[heap newBufferWithLength:options:offset:]` and falls back to a plain buffer if that fails.
  - Evidence: LIVE (the failure path is logged).
  - pvg-vulkan's `OBJECT_TYPE_*` list (`runtime/decode/resource/mod.rs` ~76-86) has no type 14 (`decode_descriptor` returns "unknown type"), so the vendored snapshot has no support. libapplegfx has no object-type switch for 14 either. Single-source claim: medium confidence.
- **19 = "pooled buffer".** Two sources, with differing descriptions.
  - pvg-vulkan `resource/mod.rs` ~83-86: command-buffer-owned pooled storage. The descriptor is the compact {allocation_size u64, virtual_page u32}. It is "used for small information replies and other encoder scratch allocations". Evidence: LIVE.
  - Henge ~1153-1157, comment: "macOS 27: a buffer, with the same {length, page, flags} entry as type 1 (16 KB ones seen). The guest has object info (tags 450...) written into these." Leaving them out broke argument buffers: "black layers" and repeated "info 450 ... no buffer". Henge uses its plain `makeBuffer`.
  - Both agree that 19 is a buffer-like object whose pages are written by the host, and that the Info replies target it. They disagree on whether a flags word follows (a Henge claim). It is unverified whether the two layouts are compatible.
  - Other types seen: 1 buffer, 2/3 texture, 6 function, 7 container (sampler, depth-stencil, pipelines), 8 texture view (Henge: linear texture, alias 9), 11 IOSurface (Henge: mapped texture), 13 heap (Henge only).
  - Note: libapplegfx's Info 0x1cc takes an `objectType` word, but it only echoes `ref`.

### 2.10 Info commands in the 450 series (0x1c2-0x1d5)

**All three sources agree on the structure.** Each record is `{u32 ref, u32 buffer_id, u64 reply_offset, ...}` and the host writes a reply into a guest buffer. Exceptions: 0x1c3 has buffer_id at +0x20, 0x1ca/0x1cb have it at +0x1c, and 0x1cc has an `objectType` word between ref and buffer_id.

Opcode map (decimal = hex):

| Decimal | Hex | Meaning |
|---|---|---|
| 450 | 0x1c2 | Compute pipeline info |
| 451 | 0x1c3 | Heap texture size and align |
| 452 | 0x1c4 | Rasterization-rate map info |
| 453 | 0x1c5 | Copy rate parameter buffer (no reply) |
| 454/455 | 0x1c6/0x1c7 | Map screen to physical / physical to screen |
| 456 | 0x1c8 | Map physical to screen multiple (excluded in pvg-vulkan) |
| 457 | 0x1c9 | Render pipeline info |
| 458/459 | 0x1ca/0x1cb | Render/compute pipeline imageblock memory length |
| 460 | 0x1cc | Object unique identifier |
| 461 | 0x1cd | Buffer host-resource info |
| 462 | 0x1ce | Texture host-resource info |
| 463 | 0x1cf | Heap host-resource info |
| 464 | 0x1d0 | Sampler host-resource info |
| 465 | 0x1d1 | ICB host-resource info (pvg-vulkan only) |
| 466 | 0x1d2 | Render pipeline host-resource info (pvg-vulkan only) |
| 467 | 0x1d3 | Compute pipeline host-resource info (pvg-vulkan only) |
| 468 | 0x1d4 | Depth-stencil host-resource info (pvg-vulkan only) |
| 469 | 0x1d5 | Wide heap texture size and align (pvg-vulkan only) |

- libapplegfx `src/handlers/compute/info_replies.c` handles 0x1c2-0x1d0 (15 entries) with stub or zero replies. Its header says its field notes come from "info-decoder-opcodes.md", a private file. Evidence: KEXT-RE or trace.
  - A zero `threadExecutionWidth` made the guest's SkyLight divide by zero, so 0x1c2 is forced to 32, with `maxTotalThreadsPerThreadgroup` = 1024.
  - The pipeline-info reply is {maxTotal u32, ?, threadExecutionWidth at +8, staticThreadgroupMemoryLength at +0x10}. A newer reply width is not documented.
  - The host-resource replies (0x1cd-0x1d0) are all zero. A TODO says to publish `gpuAddress` and `gpuResourceID` later.
- Henge `pvg_metal.m` ~2005-2060 (the `info` method) implements real replies:
  - 450 = {maxTotalThreads u64, threadExecutionWidth u64, staticThreadgroupMemoryLength u64, and a byte for supportsICB}.
  - 457 = {max threads u32, imageblock sample length u32, tile match byte, indirect byte}.
  - 460 = unique identifier u64.
  - 461 = buffer gpuAddress. 463 = heap gpuAddress.
  - 462/464/466/467/468 = MTLResourceID u64 + resourceIndex u64.
  - 451 and 469 = heap texture size/align written at an offset after the descriptor (v1 and v2 descriptor).
  - The reply must fit in a mapped guest buffer (28 bytes at the offset). If it does not, Henge logs "info N: ... no buffer".
- pvg-vulkan `ops/info.rs` lines 44-53, 147, 188, 223-224, 262, 324, 358-359 carries the opcode constants.

## 3. Full opcode inventories

| Source | Inventory? |
|---|---|
| libapplegfx-vulkan | **Partial.** Outer opcodes have an enum in `src/protocol/opcodes.h`, lines 98-152 (53 counted by `LAGFX_OPCODE_COUNT`). The render inner table in `render_inner_ops.c` ~935-1043 has 96 entries (0x00-0x1d, 0x2c, 0x3c, 0x65-0xa6). The blit table in `blit_inner_ops.c` has 24 (0x12c-0x141). Info 0x1c2-0x1d0. **The compute encoder opcodes 0xc8-0xe6 are absent.** Despite the name, `compute_inner_ops.c` dispatches 16 render opcodes observed on encType 0/1, with an "unknown" TRACE fallback. The README points to a private "mos-docs" for the full catalogue. |
| pvg-vulkan (medfa12) | **Best.** `crates/pvg-vulkan-wire/src/manifest.rs` (3,627 lines) is a selector-by-selector coverage manifest of the five serializer classes (PGSerializer 95, Render encoder 152, Compute 58, Blit 38, Info 21 methods). Parsed result: 392 entries, of which 177 are Covered with opcodes, 7 are CoveredNoFixedOpcode, 150 are Excluded (no wire output) and 50 are blank/unparsed rows. `regs.rs` ~337-610 lists outer FIFO opcodes up to 0x46, with a deprecated-slot list {0x03, 0x1f, 0x21, 0x23, 0x24, 0x26, 0x27, 0x29, 0x2a-0x2f, 0x32}. |
| Henge | No table. It is a switch statement with `complain:` fallbacks. `HV_PVG_OPS=1` logs the first sighting of each outer opcode. |
| reims-vgpu, metal2vulkan | None for PVG. |

## 4. Caveats and unverified points

1. 0x20 is ExecIndirect2 on the root channel per libapplegfx, but DeleteTask per pvg-vulkan and Henge. Unresolved.
2. The compute 0xd1 field order is size-first per the oracle. Henge's direct memcpy into `MTLRegion` looks inconsistent with it (INFER).
3. The tessellation opcode 0x0e (wide indexed) is unused per the oracle, but Henge and libapplegfx both name it.
4. Oracle evidence is from the Apple serializer on macOS 26.5 with capability flags forced on. A real guest may emit only a subset, and older guests may differ.
5. The type 19 layout differs between Henge ({length, page, flags}) and pvg-vulkan ({allocation_size, virtual_page}).
6. 0x43 versus 0x45: nothing public separates them, apart from "later compact".
7. Private sources cited by the repos (mos-docs, paravirt-re) could not be read.
8. Licences: Henge and pvg-vulkan are copyleft. Use only paraphrased facts. libapplegfx is MIT but its README says its docs are private.
