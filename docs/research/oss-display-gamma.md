# Display / gamma / device-info / MMIO / boot-framebuffer: licence-clean evidence

Date 2026-11-27. Read-only research. Tree /tmp/wt-integ2 (HEAD now 56f7726, census was on e9ad699), MIT upstream /tmp/ref/up @7574eac (MIT, Matthew Jackson), our logs R1 = /tmp/x231501 (run 20261126-231501: g7_lagfx_1795702501625.txt + g7_cboot_1795702501625.txt), plus 23 g7_lagfx and 23 g7_cboot logs in /mnt/user-data/uploads.
Labels: READ = read in the named file/log; INFER = my inference; conf = high/medium/low.
Source classes: OWN (our logs/dumps), MIT (upstream), APSL (apple-oss-distributions/xnu main; apple-oss-distributions/IOGraphics @7628538, APPLE_LICENSE = APSL 2.0), PUBLIC-DOC (developer.apple.com documentation pages), MIT/Apache (gfx-rs/metal-rs, Cargo licence "MIT OR Apache-2.0", used only for numeric values of public Apple enums), GPL (QEMU, architecture only, marked).
Not used for meaning: Henge original, anyos/vendored pvg-vulkan, reims-vgpu, steelbrain/*, /tmp/ref/r. Their facts stay in /tmp/oss-mmio.md, /tmp/oss-bootfb.md, /tmp/oss-display-events.md, /tmp/oss-opcodes.md and are not repeated. (An attempt to grep /home/claude/henge for the +0xe00/+0xe04 origin was blocked by the permission classifier and not retried.)
Licence caution: the task names XNU as allowed. IOGraphics has the same licence (APSL 2.0) but is not XNU. I used it only for the shape of the public IOKit gamma interface and the Apple-logo bitmaps (A.3, F.3). Drop those two bullets if that is not acceptable.
Clones/downloads deleted at the end (/tmp/ioG, /tmp/xnuf).

---------------------------------------------------------------------
## A. Gamma descriptor

### A.1 Which op: ch4 op 0x08 (the census "swap"), only its FIRST occurrence per run
Source OWN. The shell logs the op's 10 argument words as "hvgamma:" lines (g7_cboot_1795702501625.txt:254-262). The same bytes are in the lagfx raw dump (g7_lagfx_1795702501625.txt:2764, diag cmd #23).
Framing in our tree: 12-byte header {u32 opcode(low byte), u32 total length, u32 stamp} (LAGFX_CMD_HEADER_BYTES = 12, src/protocol/opcodes.h:221), so op 8 = 12 + 40 argument bytes = len 52. READ. conf high.
First swap of R1, 10 u32 words: `00000000 00000000 00000001 00000000 00118000 00000000 00004000 00000000 00000101 48400000`.

| arg bytes | first swap | later swaps | reading | evidence / conf |
|---|---|---|---|---|
| +0x00 u32 | 0 | 0 | display port (host prints "port 0"; public PGDisplay has `port`, PGDevice.newDisplay(with:port:serialNum:)) | OWN + PUBLIC-DOC. medium-high |
| +0x04 u32 | 0 | 0 | unknown ("w1"), never non-zero in 20 runs | OWN. unknown |
| +0x08 u64 | 1 | 2,1,2,... | mapping id of the presented surface; alternates 1/2 in R1, other runs 5,7,8,9,10 | OWN: census D03 and dump/index.txt "mapped task=2 ref=8 mapping=1 tag=0x2f ... surface=1920x1080 fourcc=52476841"; surface_resolved 122/122. high |
| +0x10 u64 | 0x118000 | 0 | gamma table offset | OWN: identical in all 20 runs that have the hvgamma line. high for "offset"; space in A.2 |
| +0x18 u64 | 0x4000 | 0 | length of the gamma region in bytes (one 16 KiB page) | OWN, 20/20. high that it is a length; the table itself is shorter (A.3) |
| +0x20 u32 | 0x101 (257) | 0 | entry count (or bytes {1,1,0,0}) | OWN, 20/20. medium |
| +0x24 u32 | 0x48400000 | 0 | "checksum" field, not an integer sum (A.3) | OWN, 20/20. low on meaning |

The descriptor is sent only in the first op 8 of a run; later op 8 records have bytes 0x10..0x27 all zero (20x "swap 0 ... off 0x118000 len 0x4000 count 257 sum 0x48400000" and 20x "swap 1 ... off 0 len 0 count 0 sum 0"; 20 of 23 cboot logs contain hvgamma lines, the other 3 are runs that stop before any swap). READ. conf high.

### A.2 Address space of the offset: the host's "phys@off" read uses the wrong space
Source OWN. hvgamma line 256 reads guest PHYSICAL 0x118000 and gets `1f2003d5 08a140f9 a8831df8 ...`: 0xd503201f is the AArch64 NOP, so that is kernel code, not a table. The offset is therefore not a guest physical address.
It lies in the same space as the render task's exec-buffer offsets:
- every exec: `b0[off=0x114000 len=...]` for task 2 (g7_lagfx_1795702501625.txt:2740 and others);
- library VAs of the same task: 0x134000, 0x136b84, 0x138bd4, 0x13a0b4 (dump/index.txt fn lines);
- ch4 op 0x0c sent right before the first op 8: words {0, 0x218, 0x114000, 0, 0x4000, 0} = u32 0, u32 0x218, u64 0x114000, u64 0x4000 (g7_lagfx_1795702501625.txt:2752). 0x114000 + 0x4000 = 0x118000 = the gamma offset: the two 16 KiB regions are adjacent.
INFER (medium-high): the gamma offset is a byte offset into the render task's (task 2) memory, resolvable by the same task translation used for exec buffers. The op carries no task id, only the mapping id; that mapping belongs to task 2.
Not established: what 0x218 in op 0x0c is (length? page offset? count). It sits just past the mode-list words +0x208..+0x214 of the page (INFER low).
Test to settle it (no code written here): at the first op 8, read 0x4000 bytes at task-2 offset 0x118000 through the normal task translation and log the first 64 bytes plus the numbers in A.3.

### A.3 Element format and layout
- No table content exists in any dump (no 16 KiB or 257-entry object in dump/index.txt).
- 0x4000 is page granularity, not the table size. 257 entries at plausible widths: 3 channels x 2 B = 1542 B; 3 x 4 B float = 3084 B; RGBA half = 2056 B; all fit.
- 257 = 256 + 1 (intervals plus end point). Apple-side concept (APSL IOGraphics IOFramebuffer.cpp:3318-3335): the gamma setter accepts only channelCount == 3 and dataWidth == 16 and sizes the table ((dataWidth+7)/8) * dataCount * channelCount + gammaHeaderSize; IOFramebuffer.cpp:9295-9301 reads kIOFBGammaWidthKey / kIOFBGammaCountKey / kIOFBGammaHeaderSizeKey (IOGraphicsTypes.h:1474-1475, IOGraphicsTypesPrivate.h:110). READ. This shows the Apple concept, not the paravirtual wire layout. INFER (low-medium): 3 channels x 257 x 16-bit, 1542 B.
- Checksum word 0x48400000 has 22 trailing zero bits, so it is not an integer sum or CRC of a 3 x 257 16-bit ramp (identity ramp would sum to about 25.3 million). As IEEE float32 it is exactly 196608.0 = 3 x 65536. INFER low: a float quantity or constant, not a hash. Byte view {0,0,0x40,0x48} is also possible. Needs the table bytes.
- Public API: ParavirtualizedGraphics PGDisplay and PGDisplayDescriptor have no gamma, colour or ICC member (PGDisplay.json, PGDisplayDescriptor.json fetched). The host receives a finished frame via `encodeCurrentFrame(to:texture:region:)`; the framework consumes gamma internally. A host implementation either applies it in its present pass or ignores it. READ PUBLIC-DOC. high.
- Guest-side gamma data of a different kind (OWN): dump/lib_35024e60e414abf4.bin (function A2Xghfc; "Apple metal version 32023.622", target air64-apple-macosx15.6.0) uses `CA::OGL::Metal::GammaLUTs` (members gamma_to_linear, linear_to_gamma; srgb_to_linear / linear_to_srgb helpers). That is the compositor's sRGB LUT, separate from the display-level table. MIT upstream mentions the same two ideas: examples/xgc-replay.c:33 ("sometimes the 256x2 gamma LUT") and tests/air2spirv.c:54,403 (stock shader displayPresentFragmentWithGammaTableAndColorMatrix). READ. high that two gamma concepts exist; the display-level one has never been seen applied.

### A.4 Op 0x0a (76 B, twice per run) is the colour-matrix sibling
Source OWN, g7_lagfx_1795702501625.txt:59595 and 91929. Header 12 B, then 16 words: {0, 1, 0, 0x0c, 1.0f, 0, 0, 0, 0, 1.0f, ...}: 4 control words then 12 floats with 1.0 at float indexes 0 and 5 (and, by stride, 10): a 3x3 identity matrix in 3 rows of float4 (48 B). Same bytes in all 14 runs that have it; only the stamp differs. The matrix identity part is high confidence; the purpose "colour matrix" is medium (MIT upstream calls 0x19 CmdDisplayCompositorParameters "layer-blend + gamma-related settings", src/protocol/ops_display.h:77-83; payload layout there is "conjectured").
Control words {1, 0, 0x0c} unknown (0x0c equals the +0x104 enable value, probably coincidence). First 0x0a at +17545 ms right before the second swap; second at +25773 ms right before swap #8.
The display-page copy of that matrix in our log (`state a00..a80`: 1.0 at +0xa10, +0xa24, +0xa38, byte +0xa73 = 1) is host-written, so not guest evidence.

### A.5 x86 equivalent (MIT, READ, x86 guest, different framing, no use for arm64 layout)
vchan op 0x07 "present+gamma": payload >= 36 B: +0 display index, +4 plane id, +8 surface id, +20 gamma_len; gamma ignored (src/dispatchers/channel_display_dispatcher.c:73-74, src/handlers/display/display.c:722-740). 0x19 and 0x1a (ICC) are log-only (dispatcher lines 153-175).

---------------------------------------------------------------------
## B. Display opcode 0x17 vs vchan 0x01 on ARM64

Source OWN (R1 + all logs) and MIT.
- The arm64 guest never sends 0x17 (census K15). It sends ch4 op 0x01 once per run, 20 B: {u32 0, u32 X}. X differs per run: 0x200db, 0x206a3, 0x1e9fc, 0x1feec, 0x1ead4, 0x1e5c1, 0x1ffbd, 0x205ae (8 distinct raw lines). All lie in 0x1e000..0x20700, the band of the per-run ring pfns written to 0x1030 (0x1e21d..0x1e9d4) and 0x101c (0x1e552..0x20644). conf high that X is a page frame number.
- Page shift is 14 (g7_lagfx_1795702501625.txt:8, ring_page_shift=14). R1: 0x200db << 14 = 0x8036c000, next to the ch4 ring at 0x80304000 and ch3/ch2 rings at 0x8030c000 / 0x80320000. INFER high.
- Behavioural confirmation (OWN): after op 1 the shell sees the guest write the enable word: "display enable word +104 = 0" then "= c" (g7_cboot_1795702501625.txt:129-130). So op 1 = "this is the display shared page", the arm64 counterpart of x86 0x17 "shared state page" (MIT src/protocol/ops_display.h:63-77, src/timer/timer.c:15-40 describe the x86 one: counter at +0, +0x104 == 0xC gate, +0x100 = 4). On arm64 the library never installs a page (display_ss_gpa unset); the shell owns it. conf medium-high.
- op 2 (20 B) = {0, 1}, constant in 14/14 runs, sent right after op 1. INFER medium: ack with sequence 1.
- R1 order: op1 t=+1 ms, op2 +2 ms, then ch4 is silent 16 s until op 0x0c (+16152) and the first op 8 (+16153).

---------------------------------------------------------------------
## C. Device-info TLV table (root 0x3a, ops_misc.c:292 arm64_pairs)

Request in R1: kind 0x2a, resp_qwords 0x800 (MIT handler ops_misc.c:222-225 logs it). It is the 8th and last root command; right after it the guest writes 0x1200=0 and rings channel 4 (g7_lagfx_1795702501625.txt:190-203). READ. high.
Labels come from the MIT upstream x86 table (its own names; cites an unpublished RE doc). The arm64 guest uses the same tag numbers, so labels transfer (INFER medium-high; arm64 values fit the labels: 0x25 is a Metal family number, 0x03-0x05 = 1024).
Public anchors: MTLDevice documentation and numeric values of public enums from gfx-rs/metal-rs (MIT OR Apache-2.0): src/device.rs:70-87 MTLGPUFamily Apple1=1001 .. Apple9=1009, Mac2=2002, Common1=3001, Metal3=5001; src/library.rs:350-363 MTLLanguageVersion V2_4=0x20004, V3_0=0x30000, V3_1=0x30001.

| tag | label (MIT) | x86 | arm64 | public counterpart | verdict |
|---|---|---|---|---|---|
| 0x01 | MSAASamples | 4 | 4 | supportsTextureSampleCount | same; 4x valid for Apple GPUs (INFER) |
| 0x02 | D24S8Supported | 1 | 0 | isDepth24Stencil8PixelFormatSupported | 0 is the Apple-silicon answer; plausible, medium |
| 0x03-05 | MaxThreadsPerThreadgroup W/H/D | 1024 | 1024 | maxThreadsPerThreadgroup | same |
| 0x06 | MaxThreadgroupMemoryLength | 32768 | 32768 | maxThreadgroupMemoryLength | same (32 KiB) |
| 0x07/08 | FramebufferRead / RGB10A2Gamma | 1 | 1 | feature flags | same |
| 0x09 | NativeHardwareFP16 | 0 | 1 | none | 1 is true for the Adreno 840 host (g7-gpu-caps enables float16) |
| 0x0a | DeserializerVersion | 0x20008 | 8 | none | bare 8 on arm64; MIT comment says it should "match" 0x12; mismatch unexplained |
| 0x0b | PrimitiveTypeSupport | 0x7f | 0x1ff | MTLPrimitiveType has 5 values (mask 0x1f) | bit meaning unknown |
| 0x0c | MultiplaneTextures | 0 | 1 | none | unknown |
| 0x0d / 0x28 | Linear / MinimumLinearTextureAlignment | 256 | 16 | minimumLinearTextureAlignment(for:) | 16 is the usual Apple-GPU answer; our linear path copies (lin_pull) so host alignment is not required. medium |
| 0x0e-0x11 | Heap*/BufferFromIOSurface | 1,256,1,1 | same | heap, makeBuffer(iosurface) | same |
| 0x12 | MaxMetalShaderVersion | 0x20008 | 0x20007 | NOT an MTLLanguageVersion (public max V2_4 = 0x20004, then 0x30000+) | looks like major<<16 \| minor of the AIR/bitcode version (2.7 vs 2.8). INFER low. Dumps: compiler "Apple metal version 32023.622", macosx15.6.0; air.version metadata not decoded (no llvm-dis) |
| 0x13 | SupportsSharedTextures | 0 | 1 | none direct | host cannot export textures; unverified claim |
| 0x14 | MaxVertexAmplificationCount | 1 | 2 | maxVertexAmplificationCount | Apple GPU answer; host needs multiview; census X13: not translated |
| 0x15 | ProgrammableSamplePositions | 0 | 1 | areProgrammableSamplePositionsSupported | Apple GPU true; host needs sample locations; unverified |
| 0x16 | RasterizationRateLayerCount | 0 | 2 | supportsRasterizationRateMap(layerCount:) | census: rate maps not implemented |
| 0x17-19 | TileShaders / ImageBlocks / RasterOrderGroups | 0 | 1 | areRasterOrderGroupsSupported etc. | consistent with R1 (1 tile pipeline, tags 157/161/162; host has raster-order extension per g7-gpu-feat) |
| 0x1a | MemoryOrderAtomics | 0 | 1 | none | census X12: atomics untranslated; none seen |
| 0x1b | LargeMRT | 0 | 1 | none | unverified |
| 0x1c / 0x21 | SupportFlags2023 / 2024 | 0 | 7 / 0xfff | none | bit meanings unknown |
| 0x1d | MaxTotalComputeThreadsPerThreadgroup | 1024 | 1024 | maxThreadsPerThreadgroup | same |
| 0x1e | MaxComputeLocalMemorySizes | 32768 | 0x1f | none | 0x1f in a "sizes" slot is probably a bitmask (INFER low) |
| 0x1f | MaxComputeThreadgroupMemory | 32768 | 65536 | public 32 KiB limit | exceeds the public Apple 32 KiB figure; unexplained |
| 0x20 | ComputeThreadgroupMemoryAlignment | 256 | 16 | none | Apple-like value |
| 0x22 | GpuCoreCount | 1 | 5 | none in MTLDevice (IORegistry) | any positive number accepted |
| 0x23 | MaxTextureLayers | 2048 | 2048 | array length 2048 | same |
| 0x24 | MaxPredicatedNestingDepth | 0 | 0xffffffff | none | -1 = no limit (INFER) |
| 0x25 | HostGPUFamily | 0x10005 | 0x3f1 | MTLGPUFamily.apple9 = 1009 = 0x3f1 (PUBLIC-DOC description "A17, M3, M4"; value from metal-rs) | READ: exactly apple9. high. x86 0x10005 is not a public raw value |
| 0x26/27 | ArgumentBuffersTier / MaxSamplerCount | 1 / 1024 | absent | argumentBuffersSupport | arm64 has no such pair; guest accepted |
| 0x29 | TextureWriteRoundingModes | 0 | 7 | none | bit values unknown |

Counts (recomputed from src/protocol/ops_misc.c): arm64 table = 40 pairs incl. sentinel; 13 non-sentinel tags equal the x86 value, 26 differ, 0x26/0x27 exist only in x86.
Plausibility for our configuration: the arm64 table describes an Apple9-class GPU. The guest accepted it in 14 of 14 arm64 runs. No run exists with a different table, so which entries are load-bearing is unknown. Claimed but unimplemented on the Adreno host: 0x13, 0x14, 0x15, 0x16, 0x1a, 0x1b, flags 0x1c/0x21, 0x0b bits. In R1 the guest used tile shaders and raster-order style features only; it used no atomics, no vertex amplification, no rate maps (census). Per your preference: test each by a debug toggle, one tag per run, before changing the table.
MIT x86 comment (ops_misc.c:190-210): MSAASamples 0 makes SkyLight abort and MaxMetalShaderVersion < 0x20008 falls back to 2D (x86 evidence). arm64 uses 0x20007 and still reaches the Metal compositor, so that clamp is not valid for arm64 as written.

---------------------------------------------------------------------
## D. MMIO registers (census M04, M10, M13, M14, M16, M18, M20)

OWN, R1 and 23 lagfx logs. R1 order before any command (g7_lagfx_1795702501625.txt:31-60):
R 0x1018, R 0x1014 (probe, both 0) -> W 0x1034=0xd1 -> R 0x1034 (0xd1) -> W 0x101c -> W 0x1030 -> W 0x1004 -> W 0x1010 -> W 0x1000=1 -> R 0x122c -> 8 x { R 0x100c ; W 0x1008 }.
After the 8th doorbell (0x3a): W 0x1200=0, then the first W 0x1020 (ch4).
Interrupt service: exactly the triple R 0x1018, R 0x1014, R 0x102c, in that order, 784 times in R1 (one scan of the whole log); 860 channel doorbells; 868 interrupts raised.

| reg | guest behaviour (OWN) | meaning evidence | verdict |
|---|---|---|---|
| 0x100c (M04) | 8 reads, value = previous wp (0,0x1c,0x34,...,0x84); each read comes BEFORE the next W 0x1008 (read-then-write): a consumer-pointer / free-space read. Old runs returned 0 forever (ring DMA failed on a page-shift bug) and the guest still sent 15 doorbells up to wp 0x10c | MIT x86 header names it "_rootFIFO.fifoFaultOffset" (src/protocol/protocol.h:92) | READ+INFER. Nothing wraps (max wp 0x9c) and no fault occurs, so neither "read pointer" nor "fault offset" is refuted. low |
| 0x1024, 0x1028 (M10) | 0 accesses in 23 logs | MIT: "per-device 0-arg ObjC trampoline, value discarded" (doorbell.c:185-191), name from unpublished RE | no arm64 evidence |
| 0x1034 (M13) | written 0xd1 once per run (20/20), read back once, before 0x101c/0x1030 | MIT header: read-only BINARY_VERSION (protocol.h:71); doorbell.c:193-194 notes the kext writes | 0xd1 (209) is the guest driver's own number. INFER medium that a real host holds its own version; no run shows the guest reacting to the read-back, echo works |
| 0x1038, 0x103c (M14) | 0 accesses | MIT header lists 0x1038 as last bank register | unknown |
| 0x1214, 0x1228 (M16) | 0 accesses in 23 logs | MIT labels only ("capability bits") | unknown; probably x86 EFI only |
| 0x1200 | 1 write per run, value 0, after the last root command and before the first channel doorbell | MIT label efi_boot_state | role unknown, harmless |
| 0x1220 (M17) | 2 writes per run, value 0: (1) after the ISR triple following channel 2's first task definition (R1 line 203, +6125 ms); (2) after the ISR triple following exec #1 and right before the channel-4 doorbell with op 0x0c and the first op 8 (line 2747, +16152 ms). 28 WARN lines over 14 logs, always 0 | no allowed source | INFER low: a "kick" before new activity; the second precedes the first display present. Not needed to proceed |
| 0x122c (M18) | 1 read per run right after W 0x1000. Returned 1 in 14 runs (arm64 mode) and 9 in 6 older x86-mode runs | MIT returns 9 (doorbell.c:266-267 "capability gate"). Public: PGDeviceDescriptor.displayPortCount (UInt32), PGDevice.newDisplay(with:port:serialNum:); our swaps carry port 0 | INFER medium: number of display ports (1 = one display). The 9-runs prove nothing: they failed at the ring ("ch0 drain: header DMA failed", page shift 12 vs 14), so the guest never got past the doorbell loop. In both cases the guest wrote the first doorbell right after the read |
| MSI-X vectors (M20) | only vector 0 raised (868 times); 785 ISR triples; no other vector in our data | PUBLIC-DOC PGRaiseInterrupt = `void (^)(unsigned int vector)`, "The MSI vector to raise the interrupt on", no range stated; PGDeviceDescriptor has no vector-count property | INFER medium: one interrupt, causes demuxed by the 0x1018/0x1014/0x102c triple; no per-cause vector needed |

GPL, host-side only, not usable for the MIT library: QEMU hw/display/apple-gfx-mmio.m (GPL-2.0-or-later) sets `desc.raiseInterrupt = ^(uint32_t vector){ trace(vector); schedule raise_irq_bh on irq_gfx }` (lines ~219-223): the vector is discarded and every vector pulses the same single GIC line; a second window/IRQ (irq_iosfc, 0x10000-byte window) exists for IOSurface (lines ~200-203, 246-249); hw/vmapple/vmapple.c:206-217 wires both lines to the GIC; hw/display/apple-gfx.m ~581 takes the window size from desc.mmioLength (our mmio_size 65536 matches). Architecture lesson only: on the vmapple arm64 machine PVG has one interrupt line, which agrees with the OWN data.

---------------------------------------------------------------------
## E. Display shared page fields

- Page located by op 1 (section B), one 16 KiB page.
- +0x104: the guest writes 0xC (host sees 0, then 0xc, before userland, g7_cboot lines 129-130; every cboot log). READ. high. No public API.
- +0x100: host-written event word. R1 host counter "guest_cause_reads=0" while the IRQ bit 0x1 on 0x1014 was raised 122 times (once per op 8) and the guest ISR read 0x1014 785 times. Whether that counter reflects real guest reads is a property of our shell, not verified here.
- Only +0x104 is observed changing on the guest side in our logs. +0x000 initial 1, +0xe00 (pointer) and +0xe04 (flag) are not visible in any of our logs.
- MIT x86 page map (src/handlers/display/display.c:380-430, READ, x86 only, from the maintainer's own disassembly per its comments): +0x000 u32 conn_id = 1, +0x004 mode name, +0x012 port, +0x014/+0x016 width/height, +0x018/+0x01a cursor max 64x64, +0x01c resp_code, +0x020 fb_len, +0x100 pending_online_bit (0x4), +0x104 online_event_enabled (0xC), +0x208 numPixelFormats, +0x210 pixel-format record. Nothing at +0xe00/+0xe04. So +0x000 = 1 matches "conn_id = 1" (INFER medium for arm64); +0xe00 and +0xe04 have no allowed source: unknown.
- g7-gpu-idle lines: 0 in R1 and its cboot; not usable.
- Identity matrix at +0xa10/+0xa24/+0xa38 in our log is host-written.

---------------------------------------------------------------------
## F. Before PVG: boot_args.Video, first picture, Apple logo, switch to PVG

### F.1 boot_args (APSL, READ, re-verified from the files)
apple-oss-distributions/xnu main, pexpert/pexpert/arm64/boot.h:40-47: struct Boot_Video {v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth}; boot_args (boot.h:~62-77): Revision u16, Version u16, virtBase @8, physBase @0x10, memSize @0x18, topOfKernelData @0x20, Video @0x28. So quad-word indexes baseAddr 5, display 6, rowBytes 7, width 8, height 9, depth 10, exactly what our host reads (logo-analysis.md, vm.c:48-50). v_depth packing: mask 0xFF, depth shift 0, rotate 8, scale 16, boot-rotate 24 (boot.h:49-53). pexpert/arm/pe_init.c:425-436: scale = ((v_depth>>16)&0xff)+1, rotate from bits 8-15 plus 24-31, pixel format fixed "BBBBBBBBGGGGGGGGRRRRRRRR"; pe_init.c:544-547: kPEGraphicsMode iff v_display != 0, else text mode. boot.h:55 kBootFlagsDarkBoot (bootFlags bit 0). conf high.
Consequence for our host: its check (depth & 0xff) == 32 is right but it ignores scale and rotation, which iBoot reports in the depth word, not in width/height.

### F.2 What our logs show (OWN)
- Firmware iBoot-11881.140.96, board vma2ap (g7_cboot_1795702501625.txt ~95-100). iBoot lists `image ...: type logo offset 0x5b618 len 0x374e` (lines 45 and 86): the logo is a 14158-byte image in the boot image set iBoot loads. READ. high that the logo comes from iBoot's own resources, not from the guest kernel or PVG.
- boot_args found at ram+0x7fcc000 in R1 (virtBase 0xfffffe0020a04000, physBase 0x70a04000); found after about 2 s (display absent 20 polls at 10 Hz). Video values are not logged in any run.
- Aspect: boot frames are thumbnailed 96x64, PVG frames 96x54; the thumbnail height is capped at 64, so the boot framebuffer has h/w >= 2/3 and is not 16:9 (logo-analysis.md). medium.
- Frame sources in R1 (status lines g7_cboot:361, 565, 788, final 1272): boot_args stays at 179 frames from the start, pvg_surface grows from 74 to 1394. In the 5 runs with no swap (old ring failure) the display shows boot_args for the whole run ("pvg 0 pvg_surface 0 boot_args 3629"). Boot-phase thumbnails: "mostly-black-small-content nonblack_pct=1 colors=3" (246 samples), what a small logo on black gives. INFER medium: the logo is on screen for about 17 s in R1, then PVG takes over. Only pixel statistics, never pixels, were inspected.
- Switch policy in OUR shell (host behaviour, not Apple's): at the first op 8 the PVG surface is effectively black (g7_cboot:258-263: "boot-display-fallback-v2 ... nonzero_pixels=2073600 first_value=0xff000200 max_colour_byte=2", then "PVG surface is all black; showing the boot framebuffer while it stays black"; note the 8-byte RGBA16F pixels are read as 4-byte). The shell keeps boot_args until a swap with non-black surface content. Not an Apple behaviour.
- g7_frame_2.png of R1 shows an arrow cursor drawn in the surface itself (top-left), so the guest composites its own cursor into the presented frame; no hardware cursor was needed to see it.

### F.3 Who draws the logo, and when PVG scanout starts
- iBoot draws the logo into the framebuffer named by boot_args.Video (INFER from F.2: iBoot loads a "logo" image, boot_args holds a carveout). XNU arm console: pe_init.c:198-216 PE_initialize_console(kPEGraphicsMode); spinner/progress is not started at boot (per /tmp/oss-bootfb.md 1.4, not re-derived).
- APSL IOGraphics, older/Intel path (READ): IOFramebuffer.cpp:2315-2400 IOFramebufferBootInitFB paints a 0xbfbfbf grey background (32-bit) and logo bitmaps AppleLogo.h (84x103) / AppleLogo2X.h (168x206), black variants for gIOFBBlackBoot; IOBootFramebuffer.cpp:42-60 probes only with the provider property "AAPL,boot-display" and a non-zero PE console base. This is not what the arm64 VM runs (INFER: no such provider property there); low relevance.
- PVG start of scanout, public contract (PUBLIC-DOC, PGDisplay.json): "Set modeList ... the first time you set it, the framework connects the display to the virtualized graphics card"; the host learns of mode via modeChangeHandler and frames via newFrameEventHandler / guestPresentCount / hostPresentCount / encodeCurrentFrame. Nothing is presented by the framework until the guest sets a mode and presents; what is shown before that is the host's choice. This matches our timeline: op 1/2 at t=0, enable word 0xC before userland, first present about 16 s later in R1.
- No allowed source states when the guest stops drawing into boot_args.Video. Unknown.

---------------------------------------------------------------------
## G. Established vs unknown

Established (own evidence or public/APSL/MIT, no excluded source):
1. Op 8 layout: +0x08 u64 mapping id, +0x10 u64 gamma offset, +0x18 u64 region length, +0x20 u32 count (257), +0x24 u32 checksum word. Present only in the first op 8 of a run, identical in 20/20 runs (0x118000, 0x4000, 257, 0x48400000).
2. The gamma offset is a task-relative offset (render task 2), adjacent to the 0x114000 buffer announced by op 0x0c; not a guest physical address (the host's physical read returned kernel code).
3. Op 0x0a (twice per run) carries an identity 3x3 float matrix (float4 rows); op 1 word 2 is the display page PFN (shift 14); op 2 is {0,1}; the guest writes 0xC at page+0x104 after that.
4. MMIO behaviour: 0x1034 write-then-read of 0xd1; 0x100c read-before-each-doorbell; ISR triple 0x1018,0x1014,0x102c in fixed order; only vector 0; 0x1024/0x1028/0x1038/0x103c/0x1214/0x1228 never touched; 0x1200 and 0x1220 write 0 with the timing above; 0x122c read once.
5. TLV tag 0x25 arm64 value 0x3f1 = MTLGPUFamily.apple9 (1009); tag 0x12 values are not MTLLanguageVersion values.
6. boot_args.Video layout, depth packing, graphics-mode condition (APSL); iBoot loads a "logo" image; our shell shows boot_args until the PVG surface is non-black.

Unknown / needs a measurement (all log-only):
- Gamma table bytes, element width, checksum algorithm: read 0x4000 bytes at task-2 offset 0x118000 at the first op 8.
- Whether anything depends on gamma/matrix being applied (colour only; never shown applied).
- Op 0x0c words (0x218), op 0x0a control words, op 2 word, op 8 +0x04, MMIO 0x1220 and 0x1200.
- Which TLV entries are load-bearing on arm64 (toggle one tag per run; first candidates 0x13-0x16, 0x1a, 0x1b, 0x1c/0x21, 0x0b).
- 0x12/0x0a semantics (AIR version?): decode air.version in a dump lib_*.bin.
- boot_args.Video actual values (log base, display, rowBytes, width, height, depth word); whether iBoot's carveout is 640x1136 scale 2 (external, unverified).
- Display page +0xe00/+0xe04.
- Whether 0x100c is a read pointer or a fault offset on arm64.
- Any second (IOSFC) interrupt/device on arm64: seen only in GPL QEMU architecture.

External, unverified, host-side only: QEMU apple-gfx-mmio.m / vmapple.c / apple-gfx.m facts in section D (GPL-2.0-or-later). Nothing else from excluded sources was used.
