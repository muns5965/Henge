/* [C1-4] Exercises host/ui_runner_vm.c's Android boot-branch probe end to end: panic_probe_install()'s two
   stop_pc addresses (0x10D150 / 0x10D2B8 -- the two places inside 0x10D0B8, the function whose return value the
   earlier round's probe found gates the confirmed panic call at 0x107238, where it can return 0 to its own
   caller; see docs/boot-diagnostic-contract-2026-11-15.md), core/cpu.h's HvCpu.panic_first/panic_last/panic_hits
   capture in panic_probe_hook(), core/exception.c's psci() copying them into the machine-wide snapshot, and
   host/vm.c's hv_vm_panic_probe() accessor.

   (An earlier round of this same probe watched 0x1071C4/0x107214 -- the two conditions immediately upstream of
   the panic call itself -- and a real device confirmed both branches are taken with the predicted values. A
   round before that watched 0x105F2C/0x105F9C, the panic-helper entries. See git history for those versions of
   this file and of host/ui_runner_vm.c's hook/install functions if either round ever needs to be repeated.)

   This ROM is built for VMAPPLE_FIRMWARE-relative addressing (tools/build-asm-rom.sh strips to a raw .text image
   loaded starting at devices/vmapple.h's VMAPPLE_FIRMWARE = 0x100000), so .org below places code at the EXACT
   guest addresses the real ROM's two checks inside 0x10D0B8 sit at -- this file is not a stand-in for the real
   ROM's logic, only for its two addresses.

   Sequence:
     1. visit gate1 (0x10D150, the real ROM's "cbz w0,#0,0x10D184" testing 0x10C51C's return value) with
        x0 = 0x1_00002222 (upper 32 bits deliberately nonzero, sp = 0x70020000                      (-> first hit)
     2. set x0 = 0x5_00006666 (the real ROM's "cbz x0,#0,0x10D2E0" tests the FULL 64-bit register, unlike gate1)
     3. visit gate2 (0x10D2B8) with sp = 0x70030000                                                 (-> second/last hit)
     4. PSCI SYSTEM_RESET

   A passing run must report hits=2, first.{pc=0x10d150,x0=0x2222,sp=0x70020000} (x1/x2/x3 stay 0; the hook masks
   x0 to 32 bits at this address, since the real cbz there only tests w0 -- this proves the mask is applied, not
   just that some value came through), last.{pc=0x10d2b8,x0=0x500006666,sp=0x70030000} (x0 here is the FULL
   64-bit value, unmasked, since the real cbz there tests all 64 bits -- this proves the hook does NOT mask at
   this address). Together these prove the hook reads the right width at each address, not just that it fires
   twice. */
.text
.global _start
_start:
    movz  x0, #0x2222
    movk  x0, #0x1, lsl #32        // x0 = 0x100002222: what 0x10C51C would have returned, w0-tested (32-bit)
    movz  x9, #0x0000
    movk  x9, #0x7002, lsl #16     // sp = 0x70020000
    mov   sp, x9
    b     gate1

.org 0xd150
gate1:
    nop                            // probe fires here: captures w0 only (masked to 32 bits) -> expect 0x2222
    movz  x0, #0x6666
    movk  x0, #0x5, lsl #32        // x0 = 0x500006666: what 0x104E04 would have returned, x0-tested (64-bit)
    movz  x9, #0x0000
    movk  x9, #0x7003, lsl #16     // sp = 0x70030000
    mov   sp, x9
    b     gate2

.org 0xd2b8
gate2:
    nop                            // probe fires here: captures the full 64-bit x0 -> expect 0x500006666
    movz  w0, #0x0009
    movk  w0, #0x8400, lsl #16     // w0 = 0x84000009 (PSCI SYSTEM_RESET)
    mov   x1, #0
    mov   x2, #0
    mov   x3, #0
    hvc   #0
forever:
    b     forever
