/* C1-2 (B-track addition): a synthetic firmware image for host/vm.c's real engine, not Apple firmware.
   Real AArch64 instructions (built by tools/build-frame-probe-rom.sh, run only by tools/c1vm-test.sh's existing arm64-only,
   real-engine section): they write one "boot_args" block into guest RAM, in the exact shape host/vm.c's hv_vm_boot_display
   scans for (see its comment: u16 Revision, u16 Version, u64 virtBase, physBase, memSize, topOfKernelData, then a video block
   baseAddr, display, rowBytes, width, height, depth), naming a small framebuffer elsewhere in RAM; fill that framebuffer with
   one repeated 32-bit word; then loop forever. This is how the project's only other Apple-shaped stand-in ROM (the one
   tools/c1vm-test.sh builds inline as 16384 copies of `b .`) is extended one step further: still no Apple data, no file
   access, no network, and the loop is the same "never stops on its own" shape the existing stand-in already has.

   Addresses (guest-physical, from devices/vmapple.h / devices/vmapple.c):
     the CPU starts executing here (VMAPPLE_FIRMWARE = 0x00100000); RAM starts at VMAPPLE_MEM = 0x70000000.
   Layout chosen in RAM (the caller picks --ram-mb large enough to hold all of this; tools/c1vm-test.sh uses 256):
     0x70010000  the boot_args-shaped block (RAM offset 0x10000 is a multiple of 0x4000, the stride hv_vm_boot_display's
                  scan steps by, and is past the block that would overlap this firmware's own working registers/stack-free
                  code -- this program uses no stack)
     0x70020000  the framebuffer it names: 320 x 200, row 1280 bytes (width * 4), depth 32 -- the smallest sizes
                  hv_vm_boot_display accepts (width >= 320, height >= 200)
   Field offsets inside the boot_args-shaped block (checked against host/vm.c's scan, which is the only reader):
     +0  u16 revision (2..3 accepted)         +2  u16 version (1..3 accepted)
     +8  u64 virtBase   -- top 24 bits (>>40) must read back as 0xfffffe
     +16 u64 physBase   -- must be inside [VMAPPLE_MEM, VMAPPLE_MEM + ram size)
     +24 u64 memSize    -- nonzero, <= ram size (128 MiB here, comfortably under any --ram-mb this test passes)
     +32 u64 topOfKernelData -- not read by the scan; left zero
     +40 u64 baseAddr (video)  +48 u64 display (not read by the scan; left zero)
     +56 u64 rowBytes   -- must be >= width * 4
     +64 u64 width      +72 u64 height      +80 u64 depth (low byte must read back as 32) */
.text
.global _start
_start:
    mov   x1, #0x70010000        // the boot_args-shaped block
    movz  w2, #0x0002
    movk  w2, #0x0001, lsl #16   // revision=2 (low u16), version=1 (high u16), one 32-bit store for both
    str   w2, [x1]
    str   wzr, [x1, #4]          // padding, zero
    mov   x2, #0xfffffe0000000000
    str   x2, [x1, #8]           // virtBase
    mov   x2, #0x70000000
    str   x2, [x1, #16]          // physBase = VMAPPLE_MEM
    mov   x2, #0x08000000
    str   x2, [x1, #24]          // memSize = 128 MiB
    str   xzr, [x1, #32]         // topOfKernelData, unused
    mov   x2, #0x70020000
    str   x2, [x1, #40]          // baseAddr: the framebuffer below
    str   xzr, [x1, #48]         // display, unused
    mov   x2, #1280
    str   x2, [x1, #56]          // rowBytes = width * 4
    mov   x2, #320
    str   x2, [x1, #64]          // width
    mov   x2, #200
    str   x2, [x1, #72]          // height
    mov   x2, #32
    str   x2, [x1, #80]          // depth

    // fill the framebuffer: 320 * 200 = 64000 pixels, one repeated 32-bit word (0xaabbccdd -> bytes dd cc bb aa per pixel)
    mov   x3, #0x70020000
    mov   x5, #64000
    movz  w6, #0xccdd
    movk  w6, #0xaabb, lsl #16
fill_loop:
    str   w6, [x3], #4
    subs  x5, x5, #1
    b.ne  fill_loop

forever:
    b     forever
