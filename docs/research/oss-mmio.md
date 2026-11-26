# PVG MMIO window (BAR0 + 0x1000): what public sources say about the open registers

Research date 2026-11-27. Web and public repos only. No repo changes. Code was read, not copied; quotes are short.
Fetched content is treated as data.

## Sources actually read (shallow clones, so commit = HEAD at fetch)
| Short name | Repo / commit | Licence | Notes |
|---|---|---|---|
| UP | MattJackson/libapplegfx-vulkan @ 7574eac (single squashed commit; muns5965 fork = same + README only) | MIT | Linux device model, x86 macOS guest. Files: `tests/traces/m2-m3-init-2026-04-21.trace`, `src/doorbell.c`, `src/protocol/protocol.h`, `upstream-pr/src/protocol/protocol.c` and `state.h`, `CHANGELOG.md`. The "PROTOCOL.md / re-followup-spec-gaps.md" files it cites are NOT in the repo, so its disassembly evidence cannot be checked. |
| HG | Leviidev/Henge @ 7b55730 | GPL-2.0 | `devices/pvg_lite.c` (header comment lines 3-31, plus code), `devices/pvg_mac.m`. ARM64 guest on Apple Silicon host. |
| MF | medfa12/apple-silicon-anyos-project @ 59fd73a (single commit), `vendor/pvg-vulkan/` | LICENSE at repo root and at `vendor/pvg-vulkan/` is GPL-2.0 for the root and LGPL-3.0 for the vendored dir (I saw the LGPL-3.0 header in `vendor/pvg-vulkan`; I did not check every file's SPDX line). | Rust device model + QEMU shim, ARM64 macOS 26 guest on ARM Linux/KVM. Files: `crates/pvg-vulkan/src/model/regs.rs`, `runtime/mmio.rs`, `runtime/drain/mod.rs`, `runtime/scanout/mod.rs`, `crates/pvg-vulkan-efi/src/pci.rs`. |
| RV | steelbrain/reims-vgpu @ f83b1ed | GPL-2.0+ or LGPL-3.0+ | Contains NO PVG register knowledge. It is an x86 vGPU with its own guest drivers. It only mentions `apple-gfx` as a legacy option. Ignore for this task. |
| QEMU | qemu/qemu master `hw/display/apple-gfx-pci.m`, `apple-gfx.m` | GPL-2.0+ | Pure pass-through: BAR0 reads/writes go to `-[PGDevice mmioReadAtOffset:]` / `mmioWriteAtOffset:`, and the BAR size comes from `mmioLength`. No register semantics. Same for the `apple-gfx-mmio` variant and UTM, which ships QEMU. |
| Apple docs | developer.apple.com PGDevice pages | n/a | The fetch failed (client_error). From the QEMU usage, Apple documents only the opaque read/write-at-offset methods and the MMIO length. I found no public Apple document that describes any register. Treat as: not documented. |

Evidence basis labels:
- trace = recorded guest accesses
- RE = names inferred from kext/framework symbols or disassembly (not reproducible here)
- observed-by-author = author states a live-boot observation, log not published
- advertised = a value the device model chose
- guess

## Cross-cutting findings (facts)
1. Page size differs by architecture. x86 guests use 4 KiB pages: UP computes the base as `pfn << 12`. ARM64 guests use 16 KiB pages: HG uses `<< 14` for 0x1030 and 0x101c, and MF uses page_shift 12 for x86 and 14 for arm64e (`state.rs` ~2189, `regs.rs` ~275-286). Any PFN register decoded with the x86 shift will be wrong on ARM64.
2. UP's trace is from an x86 guest (header: "docker logs mos-docker-macos-1"). So UP's "9 at 0x122c", its bit meanings and its `<<12` are x86 evidence only.
3. UP's names for 0x1200-0x1228 ("efi boot-state", "queue-control capability", "capability bits") come from "RE: PROTOCOL.md §2.1" and are not independently checkable. MF names the same offsets as an EFI framebuffer/mode block, and its names fit the trace values better (see 0x1210/0x1214).
4. MF's regs.rs header says the constants come from `apple-pv-gpu.h` and `pvg_vulkan_fifo_format.h`. These are MF's own headers, not Apple's.

## Per-register table

### 0x1200
- Claims:
  - UP: `efi_boot_state`, a write-only sink. `doorbell.c` ~220; the protocol.c read path returns 0 for 0x1200-0x1228.
  - MF: `GFX_REG_EFI_DISPLAY`, a plain stored u32 (`regs.rs:40`, `mmio.rs:58,190`). Nothing in MF interprets its value.
  - Not in HG.
- Trace: UP trace writes 1, then performs the 0x1204 read and the 0x1210-0x121c writes, then writes 0. The 1/0 bracketing looks like begin/end (or select-display-0 then deselect).
- Confidence: low for "boot state". Medium that it is a bracket or select register. The bracket reading is my inference.
- Basis: trace for the access pattern, guess for the name. Licences: MIT / LGPL-3.0.

### 0x1204
- Claims:
  - UP: the read is in the trace (`R 0x1204`, between W 0x1200=1 and the FB writes). UP returns 0 and the guest carries on.
  - MF: `GFX_REG_EFI_MODE_COUNT`, returns a constant 1 (`mmio.rs:59`, `EFI_MODE_COUNT=1`). A unit test asserts it (`model/mod.rs:315`). The neighbours are 0x1208 `MODE_SELECT` (guest-written, stored) and 0x120c `MODE_SIZE` (read, advertised). MF's comment says the mode "count of 1" means the guest "has no other mode to select".
- Reading: select display (0x1200), read the mode count (0x1204), pick a mode (0x1208), read its size (0x120c). This is my inference from MF's layout, not a stated protocol.
- The trace's single read followed by the FB programming is consistent with that reading.
- Confidence: medium-low. Returning 1 is the best-supported value. Returning 0 also lets the x86 guest proceed.
- Basis: advertised by MF, not trace-derived; behaviour with 0 trace-observed on x86. License: MIT / LGPL-3.0.
- Caution: MF's comment says its advertised mode (1920x1080) "is a choice", so the value 1 is a design decision rather than a measured guest requirement.

### 0x122c
- Claims:
  - UP: returns 9 (x86). Its comment says `AppleParavirtGPUControl::start()+0x7c` reads this u32 and branches: 0 = bail, 1..8 = "legacy", >=9 = "modern capability path". Source: `upstream-pr/src/protocol/protocol.c:282-296`, `doorbell.c:266`. The disassembly source is not public.
  - HG: returns 1; header comment "+122c number of displays" (`pvg_lite.c:11,757`). Basis: symbol and trace work, ARM64 guest.
  - MF: `GFX_REG_EFI_DISPLAY_PORTS`, returns 1 (`regs.rs:51`, `mmio.rs:70`). Its comment (`regs.rs` ~110) says the guest "clamps it into `1..=8`" and creates one display pipe (channel pipe_index+5) per port. A second comment (~961) mentions "the clamp Apple's host-side implementation applies to the same register".
- Reconciliation (inference): HG and MF agree on display count (1) for ARM64. UP's 9 is most likely clamped to 8 by the guest, which would create 8 display pipes. UP's ">=9 = modern" reading could be an artefact of the author's own disassembly. If so, 9 on x86 means "8 displays", not a capability level. I could not verify this.
- Confidence: high (about 80%) that this is a display/port count on ARM64 and that 1 is a safe value. Low that 9 means "modern".
- Basis: RE/symbols for HG and UP, observed-by-author for MF (boots to Welcome screen with 1).
- Licences: MIT / GPL-2.0 / LGPL-3.0.

### 0x1210 / 0x1214 / 0x1228
- Claims:
  - UP: "queue-control capability" / "capability bits" / "capability bits (u32 at +0x5a0)", all write-only logging stubs (`doorbell.c:224-245`). Names are RE-only.
  - MF: framebuffer start / length / stride: `EFI_FB_START` 0x1210 (u32 or u64 write; reads return the low 32 bits), `EFI_FB_LENGTH` 0x1214, `EFI_FB_STRIDE` 0x1228 (`regs.rs:44-50`, `mmio.rs:64-69,192-202,238`). MF's own EFI app programs them: start=BAR1 base, length=w*h*4, depth 32, stride=w*4 (`pvg-vulkan-efi/src/pci.rs:203-225`). The host paints the console from `efi_fb_start` when the guest relocates the console ("console relocated to 0xf1000000" in a comment).
  - MF also has 0x1224 `EFI_STRIDE_ALIGN` (read, 64) and 0x1234 `EFI_BUILTIN_CONNECTED` (read, 1), both advertised.
- Trace values (UP): 0x1210 <- 0x7e00f, 0x1214 <- 0x8ca000. 0x8ca000 = 9,216,000 = 1920 x 1200 x 4, which fits a length. That supports MF's naming over UP's "capability bits". 0x7e00f is not an aligned address, which is odd for a framebuffer start. Unresolved: it may carry flags in the low bits, or be an unrelated value in an earlier boot stage. The 1200-row figure also does not match the 1080-row mode written to 0x121c, which I cannot explain.
- MF's live-boot comment (`scanout/mod.rs` ~647) records `efi_fb_start 0x80000000` and `efi_fb_stride 0x1e00` (= 7680 = 1920*4) from a driven x86 boot. Observed-by-author.
- Individual bits: no source documents any bit of 0x1210/0x1214/0x1228. UP's "capability bits" are names only. If these are addresses/lengths/strides, as MF and the trace values suggest, then there are no capability bits to find.
- Confidence: medium-high that 0x1214 is a byte length and 0x1228 a stride; medium that 0x1210 is an FB address (the 0x7e00f oddity). Capability-bit reading: low.
- Basis: trace value arithmetic (mine), observed-by-author (MF), RE guess (UP). Licences: MIT / LGPL-3.0.

### 0x1218
- Claims:
  - UP: "setEFIFramebufferMode mode_class" (`doorbell.c:232`). RE-only name.
  - MF: `EFI_FB_DEPTH`; its EFI app writes 32 (`pci.rs`); the host stores it and nothing else uses it (`mmio.rs:66,203`).
  - Trace: UP writes 0x20 = 32.
- Reading: 0x20 equals 32 bits per pixel, which supports "depth". "Mode class" is not supported by the one value seen. Whether the guest writes anything else (e.g. a class enum whose value happens to be 0x20) is unknown.
- Confidence: medium that this is bits-per-pixel. Basis: trace value plus MF naming. Licence: MIT / LGPL-3.0.

### 0x121c
- Claims:
  - UP: `setEFIModeSelect` with packing "(H<<16)|W" (`doorbell.c:236-241`); the trace writes 0x04380780 = 0x0438 (1080) <<16 | 0x0780 (1920).
  - MF: `EFI_FB_MODE`, a guest-written u32 stored without interpretation (`mmio.rs:67,204`).
- Packing check: 0x0438 = 1080 and 0x0780 = 1920, so the high half is height and the low half is width in the trace. This is trace-derived and arithmetic-confirmed.
- CONFLICT to note: MF's separate read-only register 0x120c (`EFI_MODE_SIZE`) is advertised as (width<<16)|height (`mmio.rs:61`, `EFI_MODE_WIDTH_SHIFT=16`). That is the opposite packing, but it is a different register and an MF design choice, not an observation. Do not transfer MF's packing to 0x121c.
- Confidence: high for height<<16 | width as the guest's write format (one trace sample, x86). Basis: trace. Licences: MIT / LGPL-3.0.

### 0x102c
- Claims:
  - UP current header: `FAULT_STATUS`, non-zero means pending faults and makes the kext ISR walk the fault queue; return 0 unless a fault is queued (`src/protocol/protocol.h:68-70`).
  - UP older code and trace comment: "version/status query" (`upstream-pr/src/protocol/protocol.c:311-320`; trace line ~58-60), implemented by returning `last_completed_stamp`. The CHANGELOG repeats "last_completed_stamp returned at 0x102c". The older code's own comment says the value is "not semantically perfect, just non-zero".
  - HG: header comment "+102c faults" (`pvg_lite.c:9`).
  - MF: `GFX_REG_INTR_FAULT`, host-set, guest-read, not read-to-clear; the ISR reads it "right after 0x1018" (`state.rs:386`, `mmio.rs:50,181`).
- Reading: three sources agree on fault status; the "version/status/last stamp" description is an early guess that UP itself later corrected (protocol.h says the prior scaffold misread the ISR). Returning a stamp there would signal a fault to the guest.
- Confidence: high (about 85%) for fault status. Basis: RE/symbols plus author-observed ISR order; the UP trace only shows the read happens after 0x1018 and 0x1014. Licences: MIT / GPL-2.0 / LGPL-3.0.

### 0x1030 (FIFO base page) and the stamp page
- Claims:
  - UP: ring_base_pfn, gpa = pfn<<12; trace writes 0x23eb6 (gpa 0x23eb6000), then 0x1004=0x10000 (size), 0x1010=0x1000 (start), 0x1000=1. Layout note in the trace: "first 4 KiB is metadata".
  - HG: "+1030 FIFO buffer page (<< 14)"; the buffer holds a control area where `stamp[channel]` sits at +4*channel, then the root ring begins at 0x1010 (= 0x4000 on ARM64). Ring length = 0x1004 minus 0x1010 (`pvg_lite.c:3-14,111,141`).
  - MF: `FIFO_BASE_PAGE`; stamp slots are 4 bytes at index*4 in that same page, bounded by page size (1024 slots on 4 KiB, 4096 on 16 KiB) (`drain/mod.rs:1338-1353`, `regs.rs:1637-1651`). Test fixtures use start 0x4000, length 0x10000, ring size 0xc000 on ARM64.
  - HG's debug helper `pvg_mac.m:303` labels 0x1030 as "shared_pa" and 0x101c as "fifo_pa" (the reverse of pvg_lite.c). That is a trace helper only; the functional code (`pvg_lite.c`) is the authority.
- Facts: the stamp page IS the 0x1030 page (offset 0 of it), and the ring starts one guest page in (0x4000 on ARM64, 0x1000 on x86). That the 0x1010 start offset equals the guest page size in all three is an observation (inference: it is the control-area size).
- Also from MF: display shared page cursor position at +0xe00 and show flag at +0xe04 (`regs.rs:803-804`); HG: display shared page filled via channel 4 op 1, with seq at +200 etc.
- Confidence: high for ARM64 (<<14, stamp page = FIFO base page, ring at +0x4000). Basis: HG symbols + traces, MF author-observed boots. Licences: GPL-2.0 / LGPL-3.0 / MIT.

### 0x1220 (cursor doorbell)
- Claims:
  - HG: a write is "the display's doorbell: the pointer moved"; value ignored (`pvg_lite.c:780`).
  - MF: dual use. A write with val < 32 sets bit val in the 0x1014 display status (read-to-clear), reads the packed cursor position from the display shared page at +0xe00 (0xffffffff means none; x in the low 16 bits, y in the high 16), queues a cursor action and raises the gfx IRQ (`mmio.rs:205-228`). MF's census comment says a deferred 0x1220 write happened "once at boot" in one measurement window (so not necessarily once per move; the census only counts deferred writes).
  - UP: no handler (0x1220 falls in its 0x1200-0x1228 stub range).
- Confidence: medium-high that writing 0x1220 notifies the host of a cursor/display event. MF's val<32 = display index is an MF reading; the HG model ignores the value. Basis: symbols/observed-by-author, no published trace. Licences: GPL-2.0 / LGPL-3.0.

### 0x1000-0x1034 neighbours (for context; not asked but they interact)
- 0x1014 = display IRQ bits, 0x1018 = stamp IRQ bits (both read-and-clear, bit N = display / channel N). 0x1020 = child channel doorbell, 0x1034 = protocol version negotiated by write then read-back. UP header and MF agree (UP's earlier "stamp cell" naming was corrected by UP itself).

## What this means for the ARM64 path
- 0x122c: 1 is supported by two ARM64 implementations (HG, MF) as the display count. The "9" is an x86 artefact; if it is clamped to 8 it would create 8 display pipes on ARM64.
- 0x1204: 1 (mode count) is the MF choice; 0 is tolerated on x86 per the UP trace. Neither is trace-proven for ARM64.
- 0x1210/0x1214/0x1228: treat as guest-written EFI framebuffer start/length/stride, not capability bits. Reads should return what the guest wrote.
- 0x121c: guest writes height<<16 | width. 0x1218 is 32 (bpp) in the only trace.
- 0x1030: ARM64 shift is 14; stamp[i] at +4*i of that page; ring at +0x4000.
- 0x102c: fault status, return 0 unless a fault is queued; not a stamp.
- 0x1220: doorbell for display/cursor events.

## Gaps and unverified items
- Nothing public documents bit-level meanings for 0x1210/0x1214/0x1228. The "capability" naming is unverified and contradicted by the trace values.
- 0x1200's meaning ("boot state" vs display select) is a guess either way.
- The 0x7e00f value and the 1200-row length arithmetic in the UP trace are unexplained.
- No ARM64 raw trace is published by any source. Everything ARM64 comes from code comments by the authors (HG, MF).
- The UP RE documents are not in the repo; Apple's docs and QEMU say nothing on registers; RV has no relevant content.
- I did not run any code or boot anything.
