# Public implementations: macOS arm64 boot logo, boot framebuffer, PVG switch-over

Research date 2026-11-27. Web-only; shallow clones were read in /tmp/r1 and /tmp/r2 and no repo was modified.
All fetched content was treated as untrusted data. No code is copied; everything is paraphrased.
Evidence labels: READ = I read the file at that commit. SEARCH = web page or search snippet only. INFER = my inference.

## 0. Bottom line (INFER, built on the facts below)

1. The Apple logo on an arm64 Mac is not drawn by XNU or by PVG or WindowServer.
   - iBoot draws it into the framebuffer that boot_args.Video names.
   - XNU's console code afterwards draws only a progress bar, on macOS builds.
   - XNU does not show the spinner during boot. It sets that up only at kPEReleaseScreen.
2. On the QEMU vmapple machine nothing scans out the boot_args framebuffer.
   - Ambrosia, an open iBoot replacement for vmapple, says so: "Nothing shows that framebuffer."
   - The carveout it describes is 640x1136, depth 32, scale 2, in guest RAM at the top of RAM.
   - Unless your host reads the Video.v_baseAddr region itself and shows it, you will never see the logo.
   - The same applies to a custom hypervisor.
3. Public PVG device models (QEMU apple-gfx, Henge pvg_lite) create the host surface only when the guest driver announces a display mode.
   - In QEMU that is modeChangeHandler. In Henge it is the first swap.
   - Before that the host console is empty or black. QEMU and reims both document this state.
4. The EFI mode registers (0x1200..0x1228) belong to the x86 PCI AppleParavirtGPU, which has a UEFI GOP.
   - Henge boots macOS arm64 to the lock screen on the MMIO variant with no EFI-bank emulation except 0x122c (number of displays) and 0x1220 (display doorbell).
   - Two projects disagree on what 0x1218 and 0x121c mean (section 3).
5. Black or corrupted PVG frames: public causes found are in section 6.
   - Pixel format: Henge sees an RGBA16F ('RGhA') display surface, not BGRA8.
   - Colour: the gamma descriptor rides in the swap command.
   - Display-page state: the shared page words, such as the identity colour matrix.

## 1. Where and when the logo is drawn

| # | Claim | Source (repo@commit, file:line) | Licence | Confidence | Evidence |
|---|---|---|---|---|---|
| 1.1 | XNU decides graphics vs text console from boot_args.Video.v_display. On arm, v_display nonzero gives kPEGraphicsMode, and zero gives text. The console base, rowBytes, width and height come straight from boot_args.Video. | apple-oss-distributions/xnu@f6217f89, pexpert/arm/pe_init.c ~L412-436 and ~L536-548 | APSL 2.0 | High | READ |
| 1.2 | boot_args.Video.v_depth is packed. Bits 0-7 are depth, bits 8-15 rotate, bits 16-23 scale minus 1, and bits 24-31 are extra boot rotate. The pixel format is hard-set to BGR byte order, a 24-char "BBBBBBBB..GGGG..RRRR" string. | same file L429-436; pexpert/pexpert/arm64/boot.h L40-53 (kBootVideoDepth*Shift) | APSL 2.0 | High | READ |
| 1.3 | XNU's console init (osfmk/console/video_console.c, initialize_screen) takes width, height, depth, rowBytes, rotate and scale from boot_vinfo. v_display is used as the type only on x86; on arm the type is 0. If v_baseAddr has bit 0 set, or the VM is not up, it is treated as a physical address and mapped. On vmapple-style platforms the mapping is write-combined. If both base and phys are zero it forces serial mode ("No video"). | same repo, osfmk/console/video_console.c ~L2797-2900 | APSL 2.0 | High | READ |
| 1.4 | The kernel draws a progress meter (macOS builds), not an Apple logo. PE_initialize_console deliberately does not start the spinner at boot ("we don't show the progress indicator on boot"). It sets up default_progress only at kPEReleaseScreen. | pexpert/arm/pe_init.c ~L225-240; video_console.c L1318, ~L2400-2600 | APSL 2.0 | High | READ |
| 1.5 | Ambrosia measured the same on QEMU vmapple: the kernel draws its progress bar, centred near the bottom, "draws no Apple logo and doesn't clear the screen". `-v` only changes this for the log, not the logo. | steelbrain/ambrosia@1b924d95, docs/graphics-drivers.md ~L249-254 | GPL-2.0+ OR LGPL-3.0+ | High | READ |
| 1.6 | Without a graphics-driver module, Ambrosia fills boot_args.Video with iBoot's carveout framebuffer: 640x1136, 2560 bytes per row, depth 32, scale 2. It states "Nothing shows that framebuffer". With a module it supplies the module's framebuffer at depth 32, scale 1 and the kernel console draws there. The carveout is at the top of RAM; the kernel's memSize ends at it. | steelbrain/ambrosia@1b924d95, docs/boot-process.md ~L258-285 and L507-512 | same | High for Ambrosia behaviour. Medium that stock iBoot or AVPBooter matches. | READ |
| 1.7 | Under Apple's own Virtualization.framework, users see an Apple icon with a progress bar during update reboots, then black. A black window with "PGDisplayNub Created" and no display-mode change means the guest never started WindowServer or PVG. This suggests the host framework does show the boot framebuffer. | utmapp/UTM issue 5307 (commenter report); developer.apple.com/forums/thread/848707 | n/a (forum) | Medium-low, anecdotal | SEARCH via WebFetch |
| 1.8 | The Apple logo is drawn by iBoot, and by boot.efi on x86. Nothing in the public sources says WindowServer or loginwindow draws it. WindowServer takes over later, and reims notes the first present clears the "boot screen". | INFER from 1.1-1.6, plus reims design.md ~L4646-4650 | n/a | Medium (negative evidence) | INFER |

## 2. How emulators expose the boot framebuffer

| # | Claim | Source | Licence | Confidence | Evidence |
|---|---|---|---|---|---|
| 2.1 | QEMU upstream vmapple and apple-gfx have no boot framebuffer. The only host surface is created in set_mode, called from PGDisplay's modeChangeHandler. The texture is BGRA8Unorm and QEMU's console surface is 32-bit. Default modes are 1920x1080@60, 1440x1080 and 1280x1024. The vmapple machine only wires apple-gfx-mmio at the APV_GFX and IOSFC MMIO windows. | qemu/qemu master, hw/display/apple-gfx.m ~L35-40, L357-385, L640-690; hw/vmapple/vmapple.c create_gfx ~L206-217. The commit hash was not fetchable because the GitHub API was blocked. | GPL-2.0+ | High | READ |
| 2.2 | The QEMU vmapple doc describes launching with Apple's AVPBooter plus aux/disk images. It says nothing about the logo or a boot framebuffer. | docs/system/arm/vmapple.rst (SPDX GPL-2.0+) | GPL-2.0+ | High (negative) | READ |
| 2.3 | ChefKiss QEMUAppleSilicon targets iOS and iBoot on T8030, T8101 and S8000, not macOS guests. It allocates a framebuffer carveout in RAM and maps it into its emulated display pipe. Its framebuffer base points at the END of the VRAM block, "the start is used for GP stuff". The base it passes in boot_args.Video is shifted by that offset so verbose boot text lands there. This is the iBoot-style handoff model. | ChefKissInc/QEMUAppleSilicon@cc4302a9, hw/arm/apple-silicon/t8030.c ~L601-653; hw/display/apple_displaypipe_v4.c ~L1000-1012 | GPLv3 with QEMU's GPLv2+ parts | High for what it does. Low relevance to macOS PVG. | READ |
| 2.4 | reims-vgpu on x86 has an option ROM that gives UEFI a GOP framebuffer in BAR2. Its guest kext snapshots the boot console from XNU's PE_Video (v_baseAddr, v_rowBytes, v_width, v_height, v_depth) into an `RgfxBootFramebuffer` registry property before IOFramebuffer moves the console onto its own framebuffer. A boot with no GOP mode shows Address 0. | steelbrain/reims-vgpu@f83b1ed0, docs/graphics-kext/design.md L3561-3569; guest/kext/src/RgfxBootConsole.hpp; ReimsGraphics.cpp ~L2163-2172 | GPL-2.0+ OR LGPL-3.0+ | High | READ |
| 2.5 | reims arm64 path: its firmware module for Ambrosia returns a framebuffer the early console draws on. Ambrosia hands it to the kernel in boot_args.Video, and the kernel's console keeps drawing there. The framebuffer is in a PCI BAR, 32 bpp 0x00RRGGBB little-endian (B first in memory), base and stride multiples of 4, stride at least 4*width. The kernel keeps a BAR the module placed inside the window, short of 0xffffffff. | steelbrain/reims-vgpu firmware/ambrosia/ReimsGraphicsModule.c L1-30, L997-1005; docs/architecture.md ~L757-765; ambrosia docs/graphics-drivers.md ~L220-247 and "Where the BARs go"; docs/internals/lessons-learned.md ~L199-205 | same | High | READ |
| 2.6 | reims's arm64 guest uses its own replacement kext (IOMobileFramebuffer subclass), not stock AppleParavirtGPU. Its display handoff behaviour is not directly comparable to the stock PVG path. | reims design.md "The arm64e kext" sections | same | High | READ |
| 2.7 | Ambrosia says the display node (/arm-io display-subsystem) must exist in the device tree from the start. IOMobileFramebuffer counts the nodes once per process, and a process that counts none never looks for a framebuffer driver. A process that counts a node waits up to 30 s for a framebuffer service. | steelbrain/ambrosia docs/internals/lessons-learned.md ~L190-199; docs/configuration.md ~L272-283 | same | High | READ |
| 2.8 | The "anyos" project (medfa12/apple-silicon-anyos-project@59fd73aa) boots macOS 26 on non-Apple ARM under QEMU/KVM with a PVG-to-Vulkan host. Its pvg-vulkan tree includes an x86 EFI GOP crate (BGRA8 linear on a BAR) and an EFI register model (section 3). I found no logo handling for the arm64 path. Its README claims the Welcome and Language screens render. | vendor/pvg-vulkan/crates/pvg-vulkan-efi/src/gop.rs; README.md | root GPL-2.0, vendored pvg-vulkan is LGPL-3.0 | High | READ |
| 2.9 | Henge (Leviidev/Henge@7b557301) runs macOS arm64 on iPhone and iPad with its own CPU, MMU and devices. It boots Apple's AVPBooter, which starts iBoot, and its pvg_lite device speaks the AppleParavirtGPU wire protocol. I found no logo or boot-framebuffer code and nothing on a boot_args.Video read. Its host/dt_fixup.c does locate boot_args to find the device tree, so boot_args.Video is reachable the same way. The article says only that macOS "reaches the lock screen in about 15 seconds". | devices/pvg_lite.c, devices/vmapple.c, docs/architecture.md ~L74-100 | GPL-2.0+ | High (negative) | READ |
| 2.10 | Viper and "other macOS-on-QEMU-ARM" projects: nothing public found. UTM uses Apple's Virtualization.framework for macOS guests, so it adds no information about PVG internals. | web search | n/a | n/a | SEARCH |

## 3. EFI mode registers (0x1200 to 0x1228): two public sources disagree

| Offset | libapplegfx-vulkan (MattJackson@7574eac, src/doorbell.c ~L219-246, MIT) | pvg-vulkan (anyos, crates/pvg-vulkan/src/model/regs.rs L41-50, LGPL-3.0) |
|---|---|---|
| 0x1200 | "efi boot state", log only | EFI_DISPLAY |
| 0x1204 | not named | mode count (device returns 1) |
| 0x1208 | not named | mode select (guest writes) |
| 0x120c | not named | mode size, returned as (W<<16)\|H; the advertised mode is 1920x1080 |
| 0x1210 | "queue control" | FB start (u64 capable) |
| 0x1214 | "capability bits" | FB length |
| 0x1218 | setEFIFramebufferMode, "mode_class" | FB depth |
| 0x121c | setEFIModeSelect, with H in the upper half and W in the lower half | FB mode |
| 0x1220 | not named | display doorbell and cursor sample (Henge's pvg_lite.c L780 also treats 0x1220 as the display doorbell) |
| 0x1224 | not named | stride alignment (device returns 64) |
| 0x1228 | "capability bits 2" | FB stride |
| 0x122c | read returns 9 ("cap gate") | number of display ports; Henge returns 1 |

- Both sources are reverse-engineered from the x86 PCI AppleParavirtGPU kext.
- The libapplegfx names are from its own RE notes (PROTOCOL.md), which are not in the repo.
- pvg-vulkan states the EFI fields are driven by the firmware GOP and that the device only advertises one fixed mode.
- Confidence: Medium-low on the field names. The two sources conflict, and I could not verify which is right.
- Henge boots the arm64 MMIO variant (apple-gfx-mmio style) without any of 0x1200 to 0x1228 except 0x122c and 0x1220.
- INFER: arm64 guests do not need the EFI bank. They are given the display via device-tree nodes and the shared-state page.
- Evidence: READ for both. The arm64 inference is from Henge's gfx_read/gfx_write, pvg_lite.c ~L750-785.

## 4. Switching from boot framebuffer to the PVG surface

| # | Claim | Source | Licence | Confidence | Evidence |
|---|---|---|---|---|---|
| 4.1 | Stock PVG host side: nothing happens until the guest driver calls the mode-change callback. QEMU then creates the surface and texture. Frames are pulled when newFrameEvent fires. A frame is dropped if the mode changed while it rendered. | qemu apple-gfx.m ~L233-345, L357-385, L660-670 | GPL-2.0+ | High | READ |
| 4.2 | The guest driver names a per-display shared state page via channel 4 (display) op 1. The device fills in modes, a mode-list sequence number, an ack-pending flag and a state word. The guest sets an event mask, reads the mode-change word, and acks with op 2. Swaps are op 8: port, mapping id, gamma offset and length, entry count, checksum. | Leviidev/Henge@7b557301, devices/pvg_lite.c header comment L1-30 and display_set_page/display_ack ~L176-225 | GPL-2.0+ | Medium-high (derived from tracing the guest driver and its symbols) | READ |
| 4.3 | x86 PCI PVG equivalent: vchan setup_shared_state writes a ~0x220-byte prefix. The 0x104 word (online_event_enabled=0xC) is called "LOAD-BEARING": the kext reads it before it will deliver the display-online event. The page also carries mode name, width, height, cursor max size, fb_len and a BGRA pixel-format record. | MattJackson/libapplegfx-vulkan@7574eac, src/handlers/display/display.c ~L360-420 | MIT | Medium (RE annotated, x86 guest) | READ |
| 4.4 | libapplegfx keeps "fallback pixels" for early boot "before macOS sets a display mode". They are served only until the live render target is ready, so the fallback does not alternate with the real frame. | libapplegfx src/display.c ~L396-432 | MIT | Medium | READ |
| 4.5 | libapplegfx holds back presenting until the draw stream has been quiet for 250 ms by default, up to a hold of 8 times that. This is because the guest recomposites in bursts of draws with no trailing swap, and mid-burst frames flicker. Setting LAGFX_DISABLE_SETTLE turns it off. | src/display.c ~L383-420 | MIT | Medium (heuristic of that project) | READ |
| 4.6 | reims: the first present from a new WindowServer clears the "boot screen". Until a new owner presents, the display shows black rather than the last owner's frame. The room is zeroed only when a new WindowServer opens the connection, never at boot. | reims design.md ~L4640-4660 (RgfxScanoutOwner.hpp) | GPL-2.0+ OR LGPL-3.0+ | High for that design | READ |
| 4.7 | reims QEMU console shows "Guest has not initialized the display (yet)" and holds its first frame when the host screen is idle and the compositor sends no frame callbacks. Do not mistake this for a guest problem. | reims docs/qemu.md ~L2431-2436 | same | High | READ |

## 5. Gamma table and colour transform in the display shared page

| # | Claim | Source | Licence | Confidence | Evidence |
|---|---|---|---|---|---|
| 5.1 | Henge fills the display page with an identity colour matrix: it writes 1.0f at +0xa10, +0xa24 and +0xa38 (the diagonal), sets byte +0xa73 = 1, and fills the mode list at +0x208/+0x210 as (H<<16\|W). The refresh rate goes at +0x214, and the physical size in mm at +0x14 and +0x24/+0x28. The page is 0x4000 bytes. | Leviidev/Henge@7b557301, devices/pvg_lite.c L176-210 | GPL-2.0+ | Medium-high (works; guest accepts it) | READ |
| 5.2 | Each swap carries a gamma-table descriptor: offset and length at +0xf80 of the page, entries and checksum at +0xf90. The table lives in task memory at the descriptor's offset. A source comment says "colour is washed out without it". | pvg_lite.c display_swap L219-263; devices/pvg_colorprobe.h L1-40 | GPL-2.0+ | Medium | READ |
| 5.3 | In Henge, WindowServer writes DISPLAY-ENCODED values (extended sRGB) into a half-float display surface. The earlier behaviour applied an sRGB encode a second time, so the screen looked pale. Fix: pass the half-float values through, unmodified. | pvg_metal.m L254-270 and L2220-2231; pvg_colorprobe.h header | GPL-2.0+ | High | READ |
| 5.4 | The display surface in Henge can be fourcc 'RGhA' (RGBA16F, 8 bytes per pixel) rather than 4-byte BGRA. Treating it as BGRA8 would read garbage. | pvg_metal.m L2409, L2612 | GPL-2.0+ | High | READ |
| 5.5 | libapplegfx treats the compositor-parameters (0x19) and ICC-profile (0x1a) commands as log-and-ack only, "cosmetic". The present-with-gamma vchan op reads display index, plane and surface id, and a gamma length at payload+20, then presents the surface as usual. The gamma is ignored. | libapplegfx src/dispatchers/channel_display_dispatcher.c ~L150-175; src/handlers/display/display.c L722-740 | MIT | Medium | READ |
| 5.6 | reims's arm64 WindowServer sends its ICC chain via set_block, one curve or matrix at a time. The chain is only the identity once all its pieces have arrived. A lesson written down: do not judge the whole chain after each piece. | reims docs/lessons.md L132; docs/graphics-kext/design.md ~L455-480 | same | High (for the reims kext path) | READ |

## 6. "No logo / black then WindowServer" reports and fixes

| # | Symptom | Cause or fix | Source | Confidence |
|---|---|---|---|---|
| 6.1 | Nothing visible before WindowServer on vmapple | No scanout of the boot_args.Video carveout. Ambrosia fixes it by providing a PCI-BAR framebuffer and an early console. | ambrosia docs/graphics-drivers.md | High |
| 6.2 | Black or garbage frames from PVG | Wrong pixel format ('RGhA' half-float vs BGRA8); wrong row stride; stale non-coherent host memory (see 6.4). | Henge pvg_metal.m; reims qemu.md ~L905-910 | Medium-high |
| 6.3 | Pale colours | Applying an sRGB encode to values that are already display-encoded. | Henge pvg_colorprobe.h | High |
| 6.4 | Sparse transparent 4x4 blocks in the boot logo, Dock and widgets on an x86 guest | Intel iGPU, Arrow Lake-S, with a host-cached memory type that is not coherent. Fixed by keeping host-copied textures out of non-coherent memory (reims commit edb5ffd8cc). Shows the boot logo is a normal composited image on that platform. | reims docs/qemu.md ~L905-910 | High for that case |
| 6.5 | macOS stops at the Apple logo with no progress bar | A host CPU issue (AMD under Hyper-V, TSC deadline timer), not graphics. | reims docs/qemu.md ~L1213-1225 | High |
| 6.6 | Display stays black after update reboot | No PGDisplay mode change. | Apple forums 848707 | Low-medium |
| 6.7 | WindowServer waits for a display that never shows up | The display-subsystem node must be in the device tree from the start. | ambrosia lessons-learned.md ~L190 | High |

## 7. Not found or unverified

- No public implementation was found that scans out an iBoot-drawn Apple logo for stock AppleParavirtGPU on arm64, other than Apple's own Virtualization.framework (closed).
- Viper and "other macOS-on-QEMU ARM" projects were not found. I do not know whether they exist publicly.
- The ChefKiss repo has no macOS or PVG display support. It is an iOS-oriented emulator.
- Which of the two EFI register maps in section 3 is right is unresolved. Neither project documents the arm64 MMIO variant's EFI bank.
- Whether stock iBoot under VZ really gives the 640x1136 scale-2 carveout is not independently confirmed. Ambrosia states it for its own replacement of iBoot.
- QEMU upstream commit hashes were not retrievable: the GitHub API is blocked in this sandbox, so I quoted master by file.
- Licence of reims-vgpu's vendored QEMU was not separately checked.

## 8. Suggested checks for the Android hypervisor (INFER, not tested)

- Log boot_args.Video at kernel entry: base, display, rowBytes, width, height, depth.
  - Decode the scale and rotation from the depth packing.
  - Treat bit 0 of the base as a physical-address flag.
  - If display is 0 the kernel runs in text mode and draws nothing.
- Read that physical range from guest RAM on your own timer and show it before the first PVG swap.
  - Expect BGRA byte order, scale 2 and possibly a 640x1136 geometry.
  - Check the stride rather than assuming width*4.
- Handle the PVG surface format per swap: check for 'RGhA' (RGBA16F) versus BGRA8 before reading pixels.
- Confirm your display shared page matches Henge's: identity matrix words, mode list, ack handshake, state word.
- Verify the display-subsystem node exists in your device tree, since WindowServer waits on it.
