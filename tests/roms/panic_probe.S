/* [C1-4] Exercises host/ui_runner_vm.c's Android boot-branch probe end to end: panic_probe_install()'s two
   stop_pc addresses, core/cpu.h's HvCpu.panic_first/panic_last/panic_hits capture in panic_probe_hook(),
   core/exception.c's psci() copying them into the machine-wide snapshot, and host/vm.c's hv_vm_panic_probe()
   accessor.

   [2026-11-21, D4 round update] The hook's first slot was retargeted from 0x10D2B8 (point A, resolved: a real
   [A] run showed it firing with a nonzero/success-branch x0) to 0x10C744 (point C: the USB wait-loop address a
   real [A] run was found parked at immediately afterward). Point C's hook rate-limits its own stdout print
   (first hit in full, then at most once every 2 seconds) because the real loop can hit this address many
   millions of times, but it still captures every hit's pc/lr/x0/sp into the same HvPanicHit structure point A
   used to, so this ROM's sequencing and the structured PANIC_DIAG snapshot check below are otherwise unchanged
   from the point-A version of this file -- only the address and the "point A"/"point C" labels moved. The
   second slot, 0x10DF9C (point B: a third, previously-unobserved call site to 0x10C51C, reached only if a
   global state-table slot is still zero; see host/ui_runner_vm.c's "[2026-11-21 round]" comment and
   docs/boot-diagnostic-contract-2026-11-15.md), is untouched by the D4 round. Point B's hook records only
   cpu->x[4] into hit.x0 (HvPanicHit has no x4/x5/x6 fields of its own) and prints x4/x5/x6 directly to stdout at
   the moment it fires, so tools/c1vm-test.sh checks the two immediate print lines for x4/x5/x6, not the final
   PANIC_DIAG snapshot, for x5/x6. This file no longer visits the retired 0x10D150/0x10D2B8 addresses (neither is
   one of the hook's two current stop_pc values, so a visit there would simply never fire the hook); see git
   history for the versions of this file and of host/ui_runner_vm.c's hook/install functions that covered those
   and earlier rounds (0x1071C4/0x107214, 0x105F2C/0x105F9C) if any of them ever needs to be repeated.

   This ROM is built for VMAPPLE_FIRMWARE-relative addressing (tools/build-asm-rom.sh strips to a raw .text image
   loaded starting at devices/vmapple.h's VMAPPLE_FIRMWARE = 0x100000), so .org below places code at the exact
   guest addresses the hook is installed at -- this file is not a stand-in for the real ROM's logic at either
   address (point C in particular is not a claim that the real ROM's 0x10C744 instruction actually tests this
   same x0/sp pair on any real boot, nor is point B a claim that 0x10DF9C is actually reached; those are separate,
   real-device questions docs/boot-diagnostic-contract-2026-11-15.md tracks), only a way to exercise the hook's
   own install/capture/snapshot/reset machinery at the two addresses it currently watches.

   Sequence:
     1. visit gateC (0x10C744) with x0 = 0x1_00002222 (upper 32 bits deliberately nonzero, to prove this point
        captures the FULL 64-bit register, unlike the retired 0x10D150 point), sp = 0x70020000   (-> first hit)
     2. set x4 = 0x6666, x5 = 0x7777, x6 = 0x8888 (three distinct fixed values, so a mix-up between them would be
        caught), sp = 0x70030000
     3. visit gateB (0x10DF9C)                                                                 (-> second/last hit)
     4. PSCI SYSTEM_RESET

   A passing run must report hits=2, first.{pc=0x10c744, x0=0x100002222, x1=x2=x3=0, sp=0x70020000} (the full,
   unmasked 64-bit value -- this proves point C is captured at full width), last.{pc=0x10df9c, x0=0x6666 (=x4,
   the only one of x4/x5/x6 the final snapshot can hold), x1=x2=x3=0, sp=0x70030000}. Separately, the immediate
   print line for point C must show hit #1 and x0=0x100002222, and the one for point B must show x4=0x6666
   x5=0x7777 x6=0x8888 -- tools/c1vm-test.sh checks both lines too, since x5/x6 never reach the structured
   snapshot. */
.text
.global _start
_start:
    movz  x0, #0x2222
    movk  x0, #0x1, lsl #32        // x0 = 0x100002222: nonzero upper 32 bits, to prove point C is unmasked
    movz  x9, #0x0000
    movk  x9, #0x7002, lsl #16     // sp = 0x70020000
    mov   sp, x9
    b     gateC

.org 0xc744
gateC:
    nop                            // probe fires here (point C): captures the full 64-bit x0 -> expect 0x100002222
    movz  x4, #0x6666
    movz  x5, #0x7777
    movz  x6, #0x8888              // three distinct values: x4 is the only one the final snapshot can hold (last.x0)
    movz  x9, #0x0000
    movk  x9, #0x7003, lsl #16     // sp = 0x70030000
    mov   sp, x9
    b     gateB

.org 0xdf9c
gateB:
    nop                            // probe fires here (point B): hit.x0 <- x4 (0x6666); x4/x5/x6 also printed directly
    movz  w0, #0x0009
    movk  w0, #0x8400, lsl #16     // w0 = 0x84000009 (PSCI SYSTEM_RESET)
    mov   x1, #0
    mov   x2, #0
    mov   x3, #0
    hvc   #0
forever:
    b     forever
