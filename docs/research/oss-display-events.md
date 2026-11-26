# Display event bits / VBL / 0x1014 / ack: public OSS findings

Method: WebFetch of raw.githubusercontent.com and api.github.com on 2026-11-27. Each page was summarised by a small model, so line numbers are approximate and function names are the reliable locator. Re-read the files directly before relying on a detail. No code copied.

## Key finding (inference first)
Henge (Leviidev/Henge, devices/pvg_lite.c) defines the display event word at +0x100 as follows:
- 0x4 = mode change
- 0x2 = swap complete
- 0x8 = refresh / VBL
- The guest-written mask at +0x104 gates 0x8.
- The code raises a VBL only when +0x104 has bit 0x8 set. The guest arms 0xC, which includes 0x8.

Our guest arms +0x104 = 0xC, so it expects 0x8 (VBL / refresh) events. Our device never sets 0x8. Henge's comments say a video wallpaper "stops when nothing else draws" without refresh events. That matches a guest that stalls after ~72 s with idle CPUs. This is inference, not proof. The Henge VBL path is labelled experimental (HV_PVG_VBLANK) and was not shown to be verified against real macOS.

## Facts

### F1. Event word +0x100 bits (Henge)
- Claim:
  - 0x4 is set by display_set_page() (mode announce).
  - 0x2 is OR'd in by display_swap() (swap completed).
  - 0x8 is OR'd in by vblank() (refresh).
  - 0x1 is not set anywhere in the file.
- Source: https://raw.githubusercontent.com/Leviidev/Henge/main/devices/pvg_lite.c (function names above; commit not retrieved).
- Licence: GPL-2.0-or-later.
- Confidence: medium-high for what the code does, low for what the guest means by it.
- Basis: inferred by the author. The comments say nothing about it being observed from Apple code. display_swap() has no comment explaining bit 2.

### F2. Mask word +0x104 (Henge)
- Claim: +0x104 is a mask the guest writes. The file header says the guest reads the event word after setting this mask. vblank() sends events only when bit 0x8 of the mask is set. The guest later acks with op 2.
- Source and licence: as F1.
- Confidence: medium. Basis: inferred from observed guest behaviour, not Apple code.

### F3. Real VBL generation (Henge)
- Claim:
  - vblank() is gated by env HV_PVG_VBLANK=1 and is experimental.
  - Rate is HV_HZ (default 60), with host ticks at 24 MHz.
  - It requires the display page to exist and mask bit 0x8 to be set.
  - It then sets event bit 0x8, counts the event and raises the GFX interrupt immediately.
  - display_set_page() also reports the refresh rate at page offset +0x214.
- Source and licence: as F1.
- Confidence: medium. Basis: inferred.

### F4. Ack protocol (Henge)
- +0xfa9 (byte): set to 1 by display_set_page() (pending ack), cleared to 0 by display_ack() when the guest sends op 2.
- +0xfbc: display_ack() stores the sequence number from the op 2 arguments.
- +0xfc0: state word, 1 when a mode is announced and waiting for ack, 2 after display_ack().
- Related page fields:
  - +0xf98: swap counter.
  - +0xfb0: mapping id.
  - +0xf80 to +0xf9f: gamma.
- Source and licence: as F1.
- Confidence: medium. Basis: inferred. The mode-announce / ack handshake is not tied to VBL in the code.

### F5. MMIO 0x1014
- Henge: the header lists "+1014 display bits" with no per-bit meaning. No code reads or sets it, and gfx_read/gfx_write just store the guest-written value. In Henge, 0x1018 is read-and-clear and is the stamps-updated bitmask (bit = 1 << channel).
- Source and licence: as F1.
- Confidence: high that Henge defines no meaning. Basis: inferred.
- MattJackson/libapplegfx-vulkan:
  - 0x1014 is the pending-displays bitmask, an atomic exchange-with-zero (read-and-clear).
  - The bit position is the display index 0..7, not the channel id.
  - 0x1018 is the same pattern for stamps (bit = stamp slot).
  - Interrupts go on MSI vector 0.
  - Sources:
    - https://raw.githubusercontent.com/MattJackson/libapplegfx-vulkan/main/src/doorbell.c (roughly lines 269-289)
    - src/protocol/state.h (roughly line 443)
  - Tree head when fetched: 7574eac0d7396a9f49366f007c7fbeb44245826c, 2026-09-06.
  - Licence: MIT (relicensed from AGPL-3.0 on 2026-07-19 per its README).
  - Confidence: high on the code. Basis: clean-room, inferred. The README says clean-room, with no Apple code.
- Implication (inference): if the display cause is a per-display bit (libapplegfx), your "0x1 once per swap" is display 0. Its meaning may be "display 0 needs service", not a swap-vs-VBL distinction. The +0x100 word carries the event type. I could not find any source that gives a different per-bit meaning for 0x1014.
- Caveat on that inference: libapplegfx never appears to set the bit. The fetched files showed no writer of pending_displays_bitmask. state.h only says the "drain path" sets it.

### F6. Online event (libapplegfx timer.c)
- Claim:
  - A ~60 Hz timer tick scans up to 8 display slots.
  - For each slot it reads the 32-bit word at shared-page +0x104. When it equals 0xC and the display is not yet in an "enabled" bitmask, it fires once.
  - It writes 0x4 to +0x100 (this is the "online" event) and raises MSI vector 0.
  - It does not touch pending_displays_bitmask.
  - The comment says firing immediately fixed an earlier deadlock, where deferring or counting the event caused timeouts.
- Source: https://raw.githubusercontent.com/MattJackson/libapplegfx-vulkan/main/src/timer/timer.c (same head as above).
- Licence: MIT.
- Confidence: medium-high. Basis: inferred. 0xC is treated as the "guest armed the online gate" signal.
- Match with our setup: this is the same one-shot behaviour we have. The cause bit 0x1 on 0x1014 is not part of it in that repo.

### F7. VBL counter page (libapplegfx ops_display.h)
- Claim:
  - The header says WindowServer polls the shared-state page (opcode 0x17) for a vblank counter and frame sequence numbers.
  - The counter is at offset +0 of that page.
  - The tick function increments it and writes it to guest memory on each 60 Hz tick.
  - The handler zeroes the page on registration, and the page counts as installed once the guest writes a non-zero address.
  - It points to "section 14.6" of an external spec. That spec is not published.
- Source: https://raw.githubusercontent.com/MattJackson/libapplegfx-vulkan/main/src/protocol/ops_display.h
- Licence: MIT.
- Confidence: medium. Basis: inferred.
- Inference: a second possible stall cause is a polled VBL counter at +0 that never advances, separate from the +0x100 event. Check whether we increment the +0 word.
- Gap: display_ack in handlers/display/display.c only logs. Its TODO says it should match frame ids, advance the vblank counter and signal present completion, and none of that is implemented.

### F8. Present-complete
- libapplegfx has no implemented present-complete signal. The ack handler is a stub (F7).
- Henge treats 0x2 as swap-complete (F1).
- Other repos I could read gave no further information.

### F9. QEMU hw/display/apple-gfx*
- Claim: QEMU treats the device as opaque. It forwards MMIO reads and writes to Apple's PGDevice on a pool thread.
- IRQ: the framework's raiseInterrupt callback schedules a bottom half that pulses an IRQ line.
- Frame pacing: pacing comes from the framework's newFrameEventHandler. The code runs no timer and no VBL logic. It caps outstanding frames at 2.
- Licence: GPL-2.0-or-later (github.com/qemu/qemu, hw/display/apple-gfx.m and apple-gfx-mmio.m, master).
- Confidence: high.
- Basis: this is real Apple code running on the host. The register semantics are hidden inside Apple's framework, so it gives no bit meanings.

### F10. medfa12/apple-silicon-anyos-project
- Only the top-level README was readable. The pvg device lives under scripts/pvg_vulkan/ (not read).
- The README mentions "completion stamps and display interrupts so macOS knows work finished", and the log pvg-vulkan-fail.log with present_content lines.
- Licence: repo GPL-2.0. The pvg device is LGPL-3.0-or-later. The translator uses the licence in vendor/metal2vulkan/LICENSE.
- Confidence: low. Unverified: no register facts.

### F11. steelbrain/reims-vgpu and metal2vulkan
- READMEs only. Nothing on display events.
- reims-vgpu possible leads: docs/api.md, docs/accuracy-ledger.md (raw fetch failed), crates/. Not read.
- metal2vulkan is a shader translator, so unlikely to be relevant.
- Licence: both GPL-2.0-or-later OR LGPL-3.0-or-later.
- Unverified.

### F12. Other sources
- UTM, Asahi/Linux notes, and Virtualization.framework write-ups: no relevant hits.
  - The gist at https://gist.github.com/dariaphoebe/fa02d26dc5f5827ad4eb41a53553caba is about kext loading and has nothing on the GPU.
  - https://l2dy.github.io/notes/Operating-System/macOS/Paravirtualized-Graphics has no register information.
- Apple developer docs PGDisplay (https://developer.apple.com/documentation/paravirtualizedgraphics/pgdisplay.md): the framework emulates hot-plug, and the first modeList set connects the display. It exposes guestPresentCount, hostPresentCount and newFrameEventHandler. No register-level information.
- Apple forum https://developer.apple.com/forums/thread/702910 (Monterey 12.3): reports redraw failures in the Parallels VM with Metal acceleration. A later reply mentions a panic in AppleParavirtGPU (AppleParavirtDisplayPipe.cpp). It gives no cause relevant to us.

### F13. Similar "guest stops submitting / WindowServer stalls without VBL" reports
- Only Henge's comment that a video wallpaper stops without refresh events (F3), and libapplegfx's deadlock note on delayed online events (F6).
- No dedicated issue or write-up found. Not verified beyond that.

## Summary for the user (my inference, ranked)
1. Emit +0x100 |= 0x8 at ~60 Hz, only while (+0x104 & 0x8) is set, and raise the IRQ. This is the strongest public lead (Henge). Combine it with the 0x1014 cause bit already used per display.
2. Increment a 32-bit VBL counter at shared-page +0 each tick (libapplegfx lead, unverified against Apple code).
3. Bit 0x1 in +0x100 has no known meaning in any public source, so I would not rely on it.
4. Op 2 ack (+0xfa9 / +0xfbc / +0xfc0) completes the mode-announce handshake. Check it is done, since +0xfc0 should reach 2.
5. All semantics here come from clean-room or empirical reimplementations, not decompiled Apple code. None is confirmed against the real device.

## Could not verify
- Commits and line numbers for Henge.
- libapplegfx docs and whitepapers (mos-docs is unpublished).
- Who writes pending_displays_bitmask in libapplegfx.
- reims-vgpu and the anyos pvg sources.
- The Debian and googlesource QEMU mirrors blocked fetching, so the QEMU facts come from the GitHub mirror.
