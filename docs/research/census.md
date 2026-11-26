# Census of what the ARM64 PVG implementation does not handle or only partly handles

Tree: /tmp/wt-integ2, branch research/g7-integ2, HEAD e9ad699 (bld/ built 2026-11-27 00:20 = HEAD time). Read-only: nothing in the tree was changed; scratch output is in /tmp/census_work.
Evidence: R1 = run 20261126-231501 (log 07b031c1-g7_lagfx_1795702501625.txt, 26 MB, trace level, dump /tmp/x231501, device Adreno 840); R2 = run 20261126-230109 (b087874c-g7_lagfx_1795701669396.txt, dump /tmp/x230109). 23 g7_lagfx and 23 g7_cboot logs scanned for cross-run counts (all lagfx logs are trace level; 17 contain commands, builds Nov 26 01:41 .. 13:46; the others stop before the first doorbell).
Method notes: AIR counts come from re-running examples/dump-translate and tools/check-dump-spirv.sh on both dumps (both: translated 40 functions, 0 failed, dump check OK 40 shaders; expected-failures file has no entries) plus a small reader over the extracted bitcode (/tmp/census_work/inv.c); llvm-dis is not installed, so call-site operand values (e.g. sample option flags) were not decoded. Dump index.txt carries no command opcodes, so command counts are from the logs. The vk 296/254 draw skips are a driver pipeline-build failure, not an opcode gap.
Class codes: A = meaning confirmed & implemented; B = meaning confirmed & unimplemented; C = partially implemented; D = implementation suspected wrong; E = meaning unknown from allowed sources (the repo's own code/docs may implement it, but the only source for the meaning is Henge/anyos or nothing). Counting rule: one table row = one item (rows group tags that share the same status and evidence).

PRIORITY SUMMARY (VBL, Stamp, IRQ, completion, initial framebuffer, cursor registers):
  VBL: no arm64 source; x86 tick unreachable (V01,V02). Display IRQ bit only per op-8 swap, set by shell API (M06).
  Stamp: cells + bitmask fully working and evidenced (S01,S02,M07); barrier slots never exercised (S03).
  IRQ: single vector 0 per completion batch (M19); other vectors unknown (M20); fault status 0x102c constant 0 (M11, S04).
  Completion: deferred cq works (S02); fault propagation absent (S04).
  Initial framebuffer: EFI mode registers never sent in arm64 (M15,M16); first scanout path unknown (F01).
  Cursor: no arm64 cursor mechanism identified (CU01); 0x1220 write value 0 x2 per run unhandled (M17); x86 cursor ops unreachable (CU02).

Provenance register. Our own source and docs repeatedly take meaning from sources you excluded. Those items are classed E here even where the code works:
  - "Apple Henge" pvg_lite.c / pvg_metal.m: doorbell.c:302,339; channel_0_dispatcher.c:64,416,424,558; channel_compute_dispatcher.c:92,176,208,246,249,402,500,543; channel_door_dispatcher.c:45; ring_common.h:113; ops_misc.c:292 (device-info table); arm64_completion.c/.h (barriers, 20000 retries, completion thread); arm64_exec_decode.c:16; arm64_resources.c:514,644,1451,1561,1634; arm64_exec_run.h:5; arm64_objects.h:5; blit_inner_ops.c:64; surface_writeback.h:5-37 (descriptor bounds); state.h:27 (512 tasks), state.h:552, state.h:698; include/libapplegfx-vulkan.h:290,389.
  - medfa12/apple-silicon-anyos-project (GPL): arm64_depth_stencil.h:15, arm64_draw_decode.h:29, arm64_sampler.h:11, arm64_pipeline.h:46 (blend/pixelFormat tokens "only the meanings were compared").
  - docs/arm64-vmapple-correspondence.md and docs/arm64-opcode-comparison.md are themselves derived from those sources; they were used here only to enumerate tag numbers, never as meaning sources.

## 1. MMIO (BAR0 0x1000..0x1fff; src/doorbell.c). R1 = run 20261126-231501 (g7_lagfx_1795702501625), 2364 reads / 877 writes

| ID | Class | Item | Evidence / count |
|---|---|---|---|
| M01 | A | 0x1000 STATUS_CONTROL write | 1 write per run (value 1, ring_armed=1). |
| M02 | A | 0x1004 ring_size write | 1 write per run (0x10000). |
| M03 | A | 0x1008 primary-ring doorbell write | R1: 8 writes (wp 0x1c,0x34,0x44,0x54,0x64,0x74,0x84,0x9c); 8 root commands parsed (root_commands=8). Wrap of the 0x10000-0x4000 root ring never happens in any log (max wp 0x9c). |
| M04 | E | 0x100c read (our arm64 code: drain root ring on read, return cumulative u32 read pointer) | R1: 8 reads, each value equals the preceding wp (0x0,0x1c,0x34,...,0x84). Log evidence supports "read pointer" behaviour, but the drain-on-read rule and the cumulative-u32 form are cited only to Henge (doorbell.c:339, channel_0_dispatcher.c:416-424,558). Upstream MIT header names the register FIFO_FAULT_OFFSET (_rootFIFO.fifoFaultOffset); upstream x86 code returns read_ptr there too. |
| M05 | A | 0x1010 ring_start_offset write | 1 write per run (0x4000); ring_base_gpa = pfn<<14 + 0x4000 decodes real root commands (0x30,0x33,0x38,0x3a), so the base is confirmed by our own log. (The "ring size minus start_offset" rule is untested: no wrap.) |
| M06 | C | 0x1014 read = display IRQ bitmask (xchg-and-clear) | R1: 785 reads, 663 return 0, 122 return 0x1 (bit 0 only; = the 122 display op-8 swaps). The library never sets the bit itself: only lagfx_device_signal_display() (device.c:442, called by the shell) does; no periodic source, bits 1..3 never seen. |
| M07 | A | 0x1018 read = stamp IRQ bitmask (xchg-and-clear) | R1: 785 reads, 762 non-zero; values 0x2 x341, 0x4 x249, 0x10 x104, 0x8 x26, 0x12 x21, 0xc x13, 0x6 x5, 0xe/0xa/0x11 x1 each, 0 x23. Matches 868 stamp_cell writes / 868 complete_stamp lines; guest never stalled (cq_ok=860). |
| M08 | C | 0x101c write (arm64 guest WRITES it; upstream header calls it read-only ROOT_PAGE_NUMBER) | 1 write per run (0x0002008d). Stored in ring_shared_page_pfn and used only by pfn_candidates_diag (4 "pfn-diag" lines); page content is never decoded. |
| M09 | A | 0x1020 channel doorbell write | R1: 860 writes, channels 1..4 only (ch 0 and 5..15 never rung). |
| M10 | E | 0x1024 / 0x1028 writes (code: "trampoline", value discarded) | 0 occurrences in all 23 lagfx logs. Meaning only in PROTOCOL.md (not in this tree). |
| M11 | C | 0x102c read (upstream header: FAULT_STATUS, non-zero = pending fault) | R1: 784 reads, ALL 0. Our arm64 path has no fault queue: the internal completion-queue FAULT state (arm64_completion.c) is never reflected here (g7-gpu-sync fault=0 in every log). Upstream x86 code instead returned last_completed_stamp. |
| M12 | A | 0x1030 ring_base_pfn write | 1 write per run (0x1e3d9). |
| M13 | D | 0x1034 BINARY_VERSION | R1: guest writes 0xd1 (log: "spec-divergent — kext write") and reads back 0xd1. Header says read-only host-supplied version; we echo the guest's own value, the host never supplies one. |
| M14 | E | 0x1038, 0x103c (shadow slots idx 14/15) | 0 accesses. Upstream header lists 0x1038 as last bank register, unnamed. |
| M15 | C | 0x1200 write (efi_boot_state) | R1: 1 write, value 0. Logged only; read-back returns 0 because doorbell_reg_index() only maps 0x1000..0x103c (idx<16), contradicting the source comment "read-back returns the shadow". |
| M16 | C | 0x1210, 0x1214, 0x1218 (setEFIFramebufferMode mode_class), 0x121c (setEFIModeSelect H<<16/W), 0x1228 writes | 0 occurrences in all 23 logs (arm64 guest never sends EFI mode). Log-only; the width/height is never forwarded to the display; reads return 0 (same idx<16 limit). |
| M17 | E | 0x1220 write (hits the only "unhandled MMIO write" WARN) | 2 writes per run, value 0x00000000; 28 WARN lines over 14 logs, all offset 0x1220 value 0. Context (R1 log lines 203 and 2747): both directly follow an ISR-style read sequence 0x1018,0x1014,0x102c; the 1st (line 203) comes after the 0x1200 write, ch4 ops 0x01/0x02 and the first task definition (ch3 0x38 + ch2 0x44, task 1); the 2nd (line 2747) right after the first 0x43 exec (ch1) completed and before the first ch4 op 0x0c / 0x08. docs/arm64-vmapple-correspondence.md calls it cursor_moved, but that comes from Henge: not usable. |
| M18 | E | 0x122c read (capability gate; we return 1 in arm64 mode, 9 in x86 mode) | R1: 1 read, returns 1, guest proceeds. The arm64 value 1 ("number of displays") is cited to Henge; upstream returns 9 for x86. |
| M19 | A | Completion interrupt: raise_interrupt(vector 0) once per completion batch | R1: irq_raised=868 = complete_stamp 868; no other vector is ever raised. |
| M20 | E | MSI-X vectors other than 0 (display / VBL / fault vectors) | 0 raised; whether the arm64 guest expects a separate display/VBL vector is not determinable from allowed sources. |

## 2. VBL / Stamp / IRQ / completion / initial framebuffer / cursor

| ID | Class | Item | Evidence / count |
|---|---|---|---|
| V01 | E | Periodic VBL (per-refresh display IRQ) for arm64 | No source exists in the library; lagfx_timer_tick_vblank() is invoked 0 times in all 23 lagfx logs (0 "timer_tick_vblank" lines; 17 of them contain arm64 commands); the guest nevertheless ran 179 s in R1 with display IRQ bit 0 raised only per swap (122). Whether macOS needs a periodic VBL cannot be settled from allowed sources. |
| V02 | A | x86 online IRQ tick (ss[+0x104]==0xC -> ss[+0x100]=4, vec 0) in src/timer/timer.c | x86-only; unreachable in arm64 (display_ss_gpa never installed because op 0x17 is never sent; 0 hits). |
| S01 | A | Stamp cell write max(target,cur+1) at (ring_base_pfn<<shift)+slot*4 | R1: 868 writes, 0 failed; slots 1..4 seen (+0 for 8 root cmds). |
| S02 | A | Deferred completion queue (stamp completes after GPU work, in order) | R1: cq_ok=860, cq_slow=0, cq_hard_timeouts=0, cq_failed_pre=0, cq_pending=0, vk_wait_timeouts=0 (hard limit 0 = off). |
| S03 | E | Barrier slots: first arg_count_8b 8-byte words of a command are {channel, stamp} gates (arm64_completion.c, 20000-retry rule) | Never exercised: arg8=0 in all 263 sampled commands of R1 and barrier_holds=0 in all 90 g7-gpu-sync lines of all logs. Layout and retry rule are cited only to Henge. |
| S04 | C | GPU fault reporting to the guest | Internal FAULT state exists (stamp completes as failed, later cmds rejected) but nothing is signalled to the guest (0x102c stays 0, no IRQ vector); counters fault_completed/fault_rejected = 0 in every log. |
| S05 | A | Stamp bitmask bit = slot index (slots 0..4) | See M07; bit 0 (root) and 1..4 all observed; slots 5+ (display pipes) never used in arm64. |
| F01 | E | Initial framebuffer / first scanout for arm64 | No EFI mode registers are ever written (M16). Observed order in R1: ch4 op 0x01 (20 B, stamp 0x100, payload 00000000 000200db), op 0x02 (20 B, payload 01000000), later 0x0c (36 B) then op 0x08 swaps (first at t=+16153 ms, stamp 0x400). The library only creates a default 1920x1080 image (pipeline_init). Which op establishes the initial scanout is not known from allowed sources. |
| CU01 | E | Cursor in arm64 mode (no cursor command or register identified) | Candidates seen: ch4 op 0x0a (76 B, 2 per run: payload 01000000 00000000 0c000000 then floats 1.0,0,0,0,0,1.0 ...), ch4 op 0x0c (36 B, 1 per run: 0x218, 0x114000, 0x40), MMIO 0x1220 (2 per run). No evidence ties any of them to a cursor. |
| CU02 | C | x86 cursor ops 0x13 (show) / 0x14 (glyph) and cursor pipeline | Implemented for x86 only (channel_display_dispatcher.c); in arm64 channels 5..15 are routed to the compute dispatcher, so 0 hits. display.c:254 leaves glyph capture as TODO (captured_len=0). cursor_pipeline/cursor_res (64x64 glyph) are created at init in every arm64 run but never drawn. |

## 3. GPU command opcodes per channel (arm64 mode). Counts = R1 trace lines (R2 = run 20261126-230109 where noted); channel routing: 0 root, 1..4 and 5..15 compute dispatcher

| ID | Class | Item | Evidence / count |
|---|---|---|---|
| R01 | A | root 0x30 CmdDefineChildChannel | R1: 5 (child ids 1,2,3,4,12); 106 over 17 logs. |
| R02 | C | root 0x33 CmdSetResourceHeap | R1: 1 ({task 0, pfn 0x1, size 0xc000}). Stored on the task entry; consumed only by the x86 object_resolver, never in arm64 (0 uses). |
| R03 | A | root 0x38 / compute 0x38 CmdDefineHostTask | R1: 1 root + 9 compute (taskIDs 0..9), all ok. |
| R04 | E | root 0x3a CmdGetDeviceInfo2 (40-entry TLV table, keys 0x01..0x29) | R1: 1 (kind=0x2a, resp 0x800 qwords, 40 pairs written). Of 40 values, 14 equal our x86 table, 26 differ; keys 0x26/0x27 of the x86 table are absent. The arm64 values are "verbatim" from Henge pvg_lite.c (ops_misc.c:292) -> no allowed source for the value meanings; the guest accepted the table. |
| R05 | C | root opcodes handled by log+ack only: 0x04,0x05,0x06,0x07,0x08,0x09,0x0b,0x0c,0x21,0x23,0x31,0x3b,0x3c,0x41,0x80,0x81,0x82 (17 opcodes; several carry "TODO Stage 30") | 0 occurrences in all 23 logs (root only ever carries 0x30,0x33,0x38,0x3a). |
| R06 | C | root opcodes with real handlers never seen in arm64: 0x00,0x01,0x02,0x03,0x0a,0x0d,0x0e,0x20,0x26,0x27,0x28,0x29,0x2a (13) | 0 occurrences; x86-era handlers, not validated against an arm64 guest. |
| R07 | E | root 0x42 CmdSynchronizeResources | 0 occurrences; handler returns LAGFX_HANDLER_ERR_NOT_IMPLEMENTED ("cache flush not implemented yet", sync.c:39). |
| R08 | E | root: any opcode not listed in R01-R07 (e.g. 0x10..0x1f, 0x2b..0x2f, 0x32, 0x34..0x37, 0x39, 0x3d..0x40, 0x43..0x7f, 0x83+) | Falls to "ch0 dispatch: unknown opcode" WARN + unknown_opcode_count; 0 occurrences (unknown_commands=0 in R1 and R2). |
| K01 | E | compute-channel 0x01 / 0x02 / 0x08 (arm64: forwarded to shell display_command) | R1: 1 / 1 / 122 on ch4 (124 handed_to_shell, 0 not_handed); R2: 1/1/208. Names set-page / ack / swap come from the Henge shell (state.h:552). Own evidence only for op 8 (see D03). |
| K02 | E | compute 0x0a (76 B) and 0x0c (36 B) on ch4 (counted ignored_known, no WARN) | R1: 2 and 1 (ignored_known=3); older builds without the case logged them as "unknown opcode" (ch4 0x0a x2, 0x0c x1 per run). Payloads quoted in CU01. |
| K03 | E | compute 0x09 (ch4, and ch 7..11 in old logs) | Not in current logs; old logs: 2 compute + 11 "display dispatch unknown opcode 0x0009" lines. In the current arm64 path it falls to the default unknown-opcode WARN. |
| K04 | E | compute 0x20 (arm64: drop the task object cache, payload {slot}) | R1/R2: 0; 30 occurrences in 3 older logs. Meaning only from Henge pvg_metal.m (channel_compute_dispatcher.c:92). |
| K05 | C | compute 0x22 CmdUnmapMemoryImmediate | R1: 32, R2: 26, 607 over 17 logs. Log+ack only: nothing is ever unmapped. |
| K06 | C | compute 0x24 CmdSetObjectList | 0 occurrences; log+ack stub. |
| K07 | A | compute 0x25 (arm64: release object {task, ref}) | R1: 265 (67 in the sampled diag), R2: 345. Corroborated by obj_table created=329 destroyed=253 reused=246. Caveat: the "remove from active_objects" part is a no-op in arm64 (removed=0 in 265/265, the set is never filled in arm64); the real effect is lagfx_a64_res_forget. |
| K08 | C | compute 0x28 IOSurfaceLookup, 0x36 channel_event, 0x39 MapMemoryImmediate, 0x3c VchanReplacePhysical, 0x3b BackingUpdate | 0 occurrences in all 23 logs. 0x28 uses the x86 handler; 0x36/0x39/0x3c are log-only (0x39 only remembers >=4 MiB maps, creates no mapping); 0x3b stores the address per ref for the gated LAGFX_M2_BACKUPD path. |
| K09 | E | compute 0x34, 0x35, 0x3e, 0x3f resource sync / invalidate / purge hints (only act when LAGFX_ARM64_EXEC=1) | 0x34/0x35: 0; 0x3e: 52 and 0x3f: 63 occurrences in 2 older logs (max 29/34 per log), 0 in R1/R2. Layouts {task,count,{ref,flags}} are cited to Henge synchronize:/invalidate:. |
| K10 | C | compute 0x37 (routed to the x86 lagfx_compute_exec_cmdbuf even in arm64 mode) | 0 occurrences. If it ever appears it runs the x86 walker on arm64 data (no arm64 branch, unlike 0x20). |
| K11 | A | compute 0x44 task object/placement lists | R1: 9 (objects=0x4000 len 0xc000, placement=0x104000 len 0x10000); resolves 329 objects later, so the layout is confirmed by our own log. |
| K12 | A | compute 0x43 exec (render + blit segments) | R1: 418 (exec_decoded=418, exec_decode_failed=0, exec_bad_size=0, exec_read_failed=0, exec_bad_seg=0, exec_no_task=0). Segments: type0 render 316, type2 blit 102 executed. |
| K13 | E | compute 0x45 (same decode path as 0x43) | 0 occurrences in all logs. |
| K14 | E | exec segment types 1 (compute), 4 (information), other types: counted seg_unsupported, not executed | seg_t1=0, seg_t4=0, seg_other=0, seg_unsupported=0 in every log: numbering only from the excluded reference. |
| K15 | C | x86 display dispatcher cases (ch>=5): 0x01,0x02,0x04,0x06,0x07,0x10..0x1a,0x1e (17 labels) incl. 0x13/0x14 cursor, 0x16 transaction3, 0x17 shared page | x86-only; unreachable in arm64 (all 0). 0x11 and 0x04 are log-ack stubs (display.c:705 "FIFO geometry storage not implemented yet"). |

## 4. Display commands seen on ch4 (arm64)

| ID | Class | Item | Evidence / count |
|---|---|---|---|
| D01 | E | op 0x01 payload {u32 0, u32 0x000200db} (20 B) | 1 per run, forwarded to shell, stamp completes. Field meanings unknown. |
| D02 | E | op 0x02 payload {u32 1, ...} (20 B) | 1 per run, forwarded. Field meanings unknown. |
| D03 | C | op 0x08 (52 B, "swap") | R1: 122; word at payload+8 resolves to a mapped guest surface 122/122 (surface_resolved=122, gpu_content=122, written_back=122, unresolved=0) = own evidence for that one field; the remaining words are not decoded (bytes 32..51 are zero in 63 of 64 sampled records); swap_events=0; display IRQ bit 0 is set once per swap (M06). |
| D04 | C | Guest surface descriptor (fourcc) check: only BGRA accepted | Older logs: "guest surface: fourcc N refused (only BGRA)" 713 times; 0 in R1/R2. |

## 5. Inner (exec) command tags — render segment (type 0), blit (type 2). Tag numbers decimal. R1 counts done/partial/unsupported

| ID | Class | Item | Evidence / count |
|---|---|---|---|
| T01 | C | Draw tags 1 (drawPrimitives compact) and 7 (drawIndexedPrimitives) | R1: tag1 263/0/0, tag7 174/296/0; 296 of 733 draw commands skipped (437 drawn), 254 of them with no_pipeline (21 vkCreateGraphicsPipelines failures, VK_ERROR_UNKNOWN(-13) on Adreno 840; 8 pipefail_* bundles in the dump). R2: built=34 failed=2, skipped 165. Only prim=3 (triangle list) seen in 256 captured draws; prim 0..4 accepted, >4 counted prim_unsupported (0). |
| T02 | A | Tags 26 (pass descriptor), 110/125 (set buffer), 111/126 (buffer offset), 112 (fragment sampler), 114 (fragment texture), 116 (set pipeline), 117 (scissor), 130 (viewport) | R1: 316, 658, 1189, 59, 314, 440, 633, 810, 645, 619 done, 0 partial/unsupported. Same 15 tags (plus 157/161/162 and blit 300) appear in all 12 logs that print tags; no other tag has ever been seen on a device. |
| T03 | C | Tile tags 157, 161, 162 (tile buffer / tile texture / dispatch-in-region) | R1: 4/2/2 done; 1 tile pipeline, dispatches=2, groups=8, simdgroup emulated on Adreno 64-wide (sg emulated=2, pinned32=0). Lane-mapping assumption is marked UNVERIFIED on hardware (translate_function.c:2044); lane-mapped kernels are skipped when subgroup size cannot be pinned to 32 (pin32=no). |
| T04 | A | Blit tag 300 (buffer to texture) | R1: 102 (t2:300=102). |
| T05 | E | Implemented but never seen on a device (37 render tags): draws {0,2,3,4,5,6,8,9,10,11,16,17} | 12 tags; layouts only from excluded reference (arm64_draw_decode.h:29). 0 occurrences. |
| T06 | E | Implemented, never seen: depth/raster state {104 depthStencilState,107 cull,108 depth bias,115 winding,119 stencil ref} | 5 tags; 0 occurrences (g7-gpu-ds dss decoded=0; no tag 4 object in any dump). |
| T07 | E | Implemented, never seen: LOD sampler binds {113,128}, vertex texture/sampler binds {127,129} | 4 tags; 0 occurrences. docs/arm64-opcode-comparison.md still lists 127/129 as missing-known (stale vs code). |
| T08 | E | Implemented, never seen: residency hints {27,134,135,137}, fences {24,25} | 6 tags, done as no-ops; 0 occurrences. |
| T09 | C | Implemented, never seen: barriers {22,23,133} | 3 tags; recorded as partial (no barrier emitted); 0 occurrences. |
| T10 | E | Implemented, never seen: tile tags {155,156,158,159,160,163,164} | 7 tags; 0 occurrences. |
| T11 | C | Tags 30,31,34,35,36 (pass-describing, ignored) and 32,33 (unsupported) | Counted partial/unsupported; 0 occurrences. |
| T12 | E | Tags known only from the excluded reference and NOT implemented: 12,13,15,18,19,20,21,101,102,103,105,106,109,118,120,121,122,123,124,131,132,136,153,154,165,166 (+32,33 above) | 26 tags (drawPatches*, ICB execute, setBlendColor, store actions, depth clip/fill mode/line width, multi-viewport/scissor, tessellation, visibility mode, vertex amplification, dynamic stride); fall to tag_unsup. 0 occurrences. |
| T13 | E | Any other tag number (28,29,37..100,138..152,167+) | tag_unsup; no meaning known; 0 occurrences (tag_over=0, bad_rec=0). |
| T14 | E | Blit tags implemented but unseen {301,302,303,306,319}; no-ops {308..311,314..317,322,323}; unimplemented {304,305,307,312,313,318,320,321} | 0 occurrences of any of them (only 300 seen). |

## 6. Object / descriptor tags (serialized payloads, texture descriptions)

| ID | Class | Item | Evidence / count |
|---|---|---|---|
| O01 | C | Serialized tag 0xe (render pipeline): decoded = root function refs, vertex attributes (fmt/offset/buffer), stride, colour attachment 0/1 pixelFormat + blend tokens | R1: 37 objects (R2: 36). Header marks it UNVERIFIED on real ARM64 data (arm64_pipeline.h:11) although the blend/pixelFormat grammar is checked against all dump descriptors. NOT decoded or NOT applied: attachment pixel format (stub_color_format_from_pass=37, the pass format is used), attachments >=2, depth/stencil attachment format, sample count / alpha-to-coverage / rasterization-enable, vertex step function and rate (decoded in the grammar but inputRate is always VERTEX, pipeline_build.c:222), multi-buffer vertex layouts (refused: multi_stream), primitive topology class, blend colour. Counters: stub blend=1, blend decoded=36 off=21 color1=34. |
| O02 | C | Serialized tag 3 (sampler, 36 B) | R1/R2: 6. State word seen 0x84000000 x3, 0x84000005, 0x04000000, 0x04000005 (own dump confirms bit31 normalized-coords and bits 0/2 filters; unnorm_applied=2). Compare function, lodAverage and mirror-clamp-to-edge are not mapped (counters compare_ignored/mirror_clamp_approx = 0 occurrences). |
| O03 | E | Serialized tag 4 (depth/stencil state, 40 B) | 0 in every dump; decoder written from the excluded reference and tested with synthetic records only (arm64_depth_stencil.h:15-17). |
| O04 | C | Serialized tag 0x33 (tile pipeline) | 1 per dump; only root tag 1 (kernel) decoded. |
| O05 | E | Other object types/tags: fences (13), ICB (0x36), rate maps (0x32), heaps (0x0d), texture views (7,8,0x1b), info queries (0x1c2..0x1d5), buffer v2 (19) | 0 seen; obj_unsupported=0, mapped_unsupported=0. Numbers only from the excluded reference (docs/arm64-opcode-comparison.md). |
| O06 | A | Object types 1 (buffer), 2 (texture), 6 (function), 7 (serialized), 8 (linear texture over buffer) | R1 dump: 99 res lines (62 type 1, 37 type 2), 43 fn lines (type 6), 44 obj lines (type 7); type 8 seen in the log (entry_type=8). |
| O07 | A | Texture description tag 0x2f (mapped texture over a guest surface) | R1 dump: 47 mapped (R2: 53), all desc_size=48, formats 115 and 80; 4 and 35 textures in the log. Decoded through the generic (v1) branch; decoded sizes/formats match the surface. |
| O08 | E | Texture description tags 0x34, 0x39 (mapped v1/v2), 0x14, 1 | Code decodes them but 0 occurrences; layouts cited to Henge makeMappedTexture. |
| O09 | C | Linear textures (type 8; tags 9 / 0x37) | Seen: entry_type=8 mtl_format=80 468x36 etc. R1 texwhy: not_mirrored=30, slot_unbound=3 (draws sampling a texture that has no mirror object: desc unbound=33). Which tag (9 or 0x37) occurs in the dumps is not recorded. |

## 7. AIR translation (src/air2spv/translate_function.c). Population: 40 translated functions in each dump (check-dump-spirv.sh: 0 failed, spirv-val vulkan1.3 OK for 40/40; dump-expected-failures.txt is empty); 94 distinct prototype names, 104 defined functions, prototype name sets identical in both dumps

| ID | Class | Item | Evidence / count |
|---|---|---|---|
| X01 | A | 92 of 94 prototype names recognised: 50 via the GLSL table, 42 via special paths (convert, dot, saturate, any/all, dfdx/dfdy/fwidth, sample/read/write_texture, unpack.unorm4x8, simd, get_simdgroup_size, implicit imageblock) | "unrecognized" trace lines: 0 for air.*; module-level counters: 0 rejected (fail closed), 0 control-flow undef sinks. |
| X02 | A | llvm.lifetime.start/end (void): dropped | 4 call sites per dump pass (2+2 trace lines). |
| X03 | C | OpUndef placeholders (unresolved operands / typed-undef contract) | "undef summary: 40 modules, 150 undef values used (R2: 151), 13 stores of possibly-undefined values, 0 reach a branch/switch/select condition". 38/40 modules use >=1. Worst: A2Xghfc (35024e60e414abf4): 13 OpUndef defs incl. a pointer-typed OpUndef and two struct undefs, 13 undef stores. Other undef sources are not attributed (LAGFX_UNDEF_DEBUG prints nothing for these modules); the i8 status byte of each air.sample_texture result (X04) is one known source. |
| X04 | C | air.sample_texture_2d/3d: only the implicit-LOD form (explicit Lod 0 in the vertex stage); trailing option operands (offset/bias/level/gradient/compare) are ignored; i8 status field = OpUndef | 317 sample call sites translated (one dump); whether any dumped call carries non-default options was not determined (no llvm-dis in the sandbox); meaning of the option operands is not documented in allowed sources. |
| X05 | D | air.sample_texture_3d.v4f16 sampled as a 2D slice (source comment: "Approximate") | 1 prototype in 1 module (the UberShader variant with sample_3d_lut). |
| X06 | D | constexpr samplers (module global __air_sampler_state / metadata air.sampler_state) replaced by a default sampler or the first bound sampler | 7 of 40 modules carry air.sampler_state (ShadowVertical/HorizontalBlur, ShadowComposite, AlphaTexture, ... ); semantically approximate per source comment (translate_function.c:4519). |
| X07 | C | Stage-link rules: fragment input without matching vertex output -> zero-filled Private; lane-count mismatch -> truncate/zero-fill | Marked UNVERIFIED (translate_function.c:1877); R1 device counters stage_link[mismatch_fixed=0 missing_zeroed=69 unlinked=0]; offline dump check: 0/0/0. |
| X08 | C | simdgroup emulation on 64-wide subgroups (air.simd_shuffle*, simd_shuffle_and_fill_down, get_simdgroup_size, thread_index_in_simdgroup) | 1 module (tile_simd_blur_15); UNVERIFIED lane mapping (translate_function.c:2044); R1 sg emulated=2. |
| X09 | C | air.store.implicit_imageblock / load.implicit_imageblock (tile function) | 1 module; runs as compute between two halves of the pass; unverified on hardware. |
| X10 | D | GLSL table naming: 73 of 124 rows use the dotted names air.fast.<op> / air.precise.<op> that the source comment itself says do not match Apple's real names; 29 rows use air.fast_<op>/air.precise_<op>, 22 plain | Only 27 rows are hit by the 92 recognised dump names (0 of the 73 dotted rows). The underscore forms are missing for sin, cos, tan, asin, acos, atan, sinh, cosh, tanh, trunc, round, step, smoothstep, distance, cross, reflect, refract, radians, degrees, ldexp; a real call such as air.fast_sin.f32 matches no row and becomes a typed OpUndef (trace "unrecognized non-void"). 0 occurrences in the dumps. |
| X11 | B | Integer/other math intrinsics (air.max/min/abs/clamp .s/.u.iN, popcount, clz, ctz, extract/insert_bits, select-style, round variants) | No handler (comment in the table cites air.max.s.i32 / air.clamp.u.i32 as the real names); 0 occurrences in the dumps. |
| X12 | B | discard_fragment, atomics, threadgroup (addrspace 3) memory, gather/compare/cube/array/1D texture forms, MSAA texture ops, argument-buffer access beyond the structs already handled | No handler found in translate_function.c (grep: no OpKill, no atomic OpAtomic*, no addrspace-3 variables); 0 occurrences in the dumps. |
| X13 | B | Entry attributes not handled: instance_id, base_vertex/base_instance, front_facing, sample_id/sample_mask, render_target_array_index, viewport_array_index, clip_distance, depth/stencil output, coverage; interpolation qualifiers flat/centroid/sample/no_perspective on user varyings | Handled: position, vertex_id, point_size, point_coord, render_target, imageblock, texture/sampler/buffer, compute thread ids. SPIR-V decorations emitted are only Block, ArrayStride, BuiltIn, NonWritable, Location, Binding, DescriptorSet, Offset (spv_builder.h:255-262): no Flat/NoPerspective/Centroid/Sample. 0 of the 40 modules carry any of the unhandled keys (air.flat/centroid/instance_id/... 0 hits; air.no_perspective appears 29x only on [[position]]). |
| X14 | A | Compute builtins (thread_position_in_grid/threadgroup, threads_per_*, threadgroup_position, simdgroup index, ...) | 1 kernel in the dump (tile_simd_blur_15): WorkgroupId, WorkgroupSize, LocalInvocationIndex, SubgroupSize, SubgroupLocalInvocationId. |
| X15 | C | LLVM instruction codes: handled = binop, cast, call, gep, load, store, shufflevec, insertval/extractval, cmp/cmp2, select/vselect, ext/insertelt, phi, br, switch, ret, unreachable, alloca, unop, freeze(58); everything else is dropped with a trace | Dump histogram (104 bodies): binop 2199, call 1199, shufflevec 1100, extractelt 732, br 636, insertelt 440, load 396, gep 336, extractval 319, cmp2 211, phi 166, cast 165, ret 104, vselect 81, insertval 46, switch 32, unop 23, store 6, alloca 2, freeze 1: all handled. Unhandled codes (fence, cmpxchg, atomicrmw, indirectbr, invoke, va_arg ...) = 0 occurrences. |
| X16 | C | Helper functions with constant/device data pointers of other shapes, other uber-shader helper call forms | 162 helper OpFunctionCall translations; rolled-back helpers: 0 in the dumps (trace "rolled back": 0). |

## 8. Pixel / vertex formats

| ID | Class | Item | Evidence / count |
|---|---|---|---|
| P01 | A | MTLPixelFormat 45 colour formats in arm64_resources.c k_fmt (R8, R16, RG8, R32, RG16, RGBA8/BGRA8 (+sRGB), RGB10A2Unorm, RG11B10, RGB9E5, RG32, RGBA16 (+Float), RGBA32) and 5 depth/stencil (250,252,253,255,260) | Dumps: only 115 (RGBA16Float, 697 cap_img + 343 att) and 80 (BGRA8Unorm, 206 / 18) in textures and attachments; R2 log: 4 textures format 115, 35 format 80. Pipelines additionally carry 70. |
| P02 | B | Colour formats with no mapping (create fails, counted TEXWHY_C_FORMAT): 40-43 (packed 16-bit), 91 RGB10A2Uint, 94 BGR10A2Unorm (recognised by the pipeline scanner), 552-555 (XR), BC/PVRTC/ETC2/ASTC families (130..), 240/241 (422), 261/262 (X32/X24 stencil) | 0 seen in dumps; meanings from the public Apple MTLPixelFormat enum / Vulkan format list. |
| P03 | D | src/vulkan/iosurface.c:39 maps MTLPixelFormat 25 to D16_UNORM (and compute_inner_ops.c:449 "25 -> D16, medium confidence") | Our own arm64_resources.c maps 25 to R16_SFLOAT and lists Depth16Unorm as 250 (public Metal enum), so the two mappers contradict each other. Legacy/x86 mapper (arm64 resources use k_fmt); it also silently falls back to BGRA8 for unknown formats. 0 occurrences in dumps. |
| P04 | A | MTLVertexFormat 3,9,27,28,29,30,31 (UChar4, UChar4Normalized, Half4, Float..Float4) | Dumps: 27 x23 and 29 x46 (R1 pso lines; R2: 22 and 44). |
| P05 | D | MTLVertexFormat 37 mapped to R32_UINT ("UInt") and 52 to B8G8R8A8_UNORM ("UChar4Normalized_BGRA") in pipeline_build.c:165-190 | Public enum values (metal-rs 0.31.0 bindings of the Apple MTLVertexFormat enum, fetched): UInt=36, UInt2=37, UChar4Normalized_BGRA=42, ShortNormalized=52. 0 occurrences in dumps. |
| P06 | B | All other MTLVertexFormat values (1,2,4..8,10..26,32..36,38..41,42,45..51,53..55) | Not mapped: if any attribute of a pipeline has an unknown format, ALL attributes fall back silently to reflected float tight-packing (use_pso_attrs=false). 0 occurrences. |

## 9. Source markers (src/ and include/, .c/.h)

| ID | Class | Item | Evidence / count |
|---|---|---|---|
| N01 | C | TODO: 39 lines / 13 files (compute_inner_ops.c 11, render_inner_ops.c 7, opcodes.h 4, info_replies.c 4, display.c 3, channel_0_dispatcher.c 3, ...) | Mostly the x86 inner-op tables (Stage 30/70 stubs) that only act when not in arm64 mode; arm64 exec hooks bypass them. |
| N02 | C | UNVERIFIED/unverified: 9 lines / 6 files (arm64_pipeline.h:11, arm64_capture.h:84, arm64_resources.h:127, arm64_resources.c:27,1491,1625, translate_function.c:1877,2044, display.c:434) | Assumptions not testable from the logs: guest changes unlisted buffers between execs; all-zero object entry = empty slot; stage-link zero fill; simd lane mapping. |
| N03 | C | FIXME: 10 lines / 7 files (air2spirv phase-3c items) and "not implemented"/ERR_NOT_IMPLEMENTED: 14 lines / 8 files (sync.c:39, util.c:21, display.c:706, shader_translate.c llc step) | Legacy x86/air2spirv-llc path; not on the arm64 path. |

## Totals

- A (meaning confirmed & implemented): 26
- B (meaning confirmed & unimplemented): 5
- C (partially implemented): 35
- D (implementation suspected wrong): 6
- E (meaning unknown from allowed sources): 33
- total items: 105

## Full list of class E items (meaning unknown from allowed sources)

- M04: 0x100c read (our arm64 code: drain root ring on read, return cumulative u32 read pointer)
- M10: 0x1024 / 0x1028 writes (code: "trampoline", value discarded)
- M14: 0x1038, 0x103c (shadow slots idx 14/15)
- M17: 0x1220 write (hits the only "unhandled MMIO write" WARN)
- M18: 0x122c read (capability gate; we return 1 in arm64 mode, 9 in x86 mode)
- M20: MSI-X vectors other than 0 (display / VBL / fault vectors)
- V01: Periodic VBL (per-refresh display IRQ) for arm64
- S03: Barrier slots: first arg_count_8b 8-byte words of a command are {channel, stamp} gates (arm64_completion.c, 20000-retry rule)
- F01: Initial framebuffer / first scanout for arm64
- CU01: Cursor in arm64 mode (no cursor command or register identified)
- R04: root 0x3a CmdGetDeviceInfo2 (40-entry TLV table, keys 0x01..0x29)
- R07: root 0x42 CmdSynchronizeResources
- R08: root: any opcode not listed in R01-R07 (e.g. 0x10..0x1f, 0x2b..0x2f, 0x32, 0x34..0x37, 0x39, 0x3d..0x40, 0x43..0x7f, 0x83+)
- K01: compute-channel 0x01 / 0x02 / 0x08 (arm64: forwarded to shell display_command)
- K02: compute 0x0a (76 B) and 0x0c (36 B) on ch4 (counted ignored_known, no WARN)
- K03: compute 0x09 (ch4, and ch 7..11 in old logs)
- K04: compute 0x20 (arm64: drop the task object cache, payload {slot})
- K09: compute 0x34, 0x35, 0x3e, 0x3f resource sync / invalidate / purge hints (only act when LAGFX_ARM64_EXEC=1)
- K13: compute 0x45 (same decode path as 0x43)
- K14: exec segment types 1 (compute), 4 (information), other types: counted seg_unsupported, not executed
- D01: op 0x01 payload {u32 0, u32 0x000200db} (20 B)
- D02: op 0x02 payload {u32 1, ...} (20 B)
- T05: Implemented but never seen on a device (37 render tags): draws {0,2,3,4,5,6,8,9,10,11,16,17}
- T06: Implemented, never seen: depth/raster state {104 depthStencilState,107 cull,108 depth bias,115 winding,119 stencil ref}
- T07: Implemented, never seen: LOD sampler binds {113,128}, vertex texture/sampler binds {127,129}
- T08: Implemented, never seen: residency hints {27,134,135,137}, fences {24,25}
- T10: Implemented, never seen: tile tags {155,156,158,159,160,163,164}
- T12: Tags known only from the excluded reference and NOT implemented: 12,13,15,18,19,20,21,101,102,103,105,106,109,118,120,121,122,123,124,131,132,136,153,154,165,166 (+32,33 above)
- T13: Any other tag number (28,29,37..100,138..152,167+)
- T14: Blit tags implemented but unseen {301,302,303,306,319}; no-ops {308..311,314..317,322,323}; unimplemented {304,305,307,312,313,318,320,321}
- O03: Serialized tag 4 (depth/stencil state, 40 B)
- O05: Other object types/tags: fences (13), ICB (0x36), rate maps (0x32), heaps (0x0d), texture views (7,8,0x1b), info queries (0x1c2..0x1d5), buffer v2 (19)
- O08: Texture description tags 0x34, 0x39 (mapped v1/v2), 0x14, 1

## Class D items (implementation suspected wrong)

- M13: 0x1034 BINARY_VERSION
- X05: air.sample_texture_3d.v4f16 sampled as a 2D slice (source comment: "Approximate")
- X06: constexpr samplers (module global __air_sampler_state / metadata air.sampler_state) replaced by a default sampler or the first bound sampler
- X10: GLSL table naming: 73 of 124 rows use the dotted names air.fast.<op> / air.precise.<op> that the source comment itself says do not match Apple's real names; 29 rows use air.fast_<op>/air.precise_<op>, 22 plain
- P03: src/vulkan/iosurface.c:39 maps MTLPixelFormat 25 to D16_UNORM (and compute_inner_ops.c:449 "25 -> D16, medium confidence")
- P05: MTLVertexFormat 37 mapped to R32_UINT ("UInt") and 52 to B8G8R8A8_UNORM ("UChar4Normalized_BGRA") in pipeline_build.c:165-190
