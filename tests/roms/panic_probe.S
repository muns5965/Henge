/* [C1-4] Exercises host/ui_runner_vm.c's Android boot-branch probe end to end: panic_probe_install()'s two
   stop_pc addresses (0x1071C4 / 0x107214 -- the two conditions this investigation found immediately upstream
   of the confirmed panic-helper call at 0x107238, see docs/boot-diagnostic-contract-2026-11-15.md), core/cpu.h's
   HvCpu.panic_first/panic_last/panic_hits capture in panic_probe_hook(), core/exception.c's psci() copying them
   into the machine-wide snapshot, and host/vm.c's hv_vm_panic_probe() accessor.

   (An earlier round of this same probe watched 0x105F2C/0x105F9C -- the panic-helper entries themselves -- and a
   real device confirmed 0x105F2C is the one reached, called from 0x107238; see git history for that version of
   this file and of host/ui_runner_vm.c's hook/install functions if that round ever needs to be repeated.)

   This ROM is built for VMAPPLE_FIRMWARE-relative addressing (tools/build-asm-rom.sh strips to a raw .text image
   loaded starting at devices/vmapple.h's VMAPPLE_FIRMWARE = 0x100000), so .org below places code at the EXACT
   guest addresses the real ROM's two upstream conditions sit at -- this file is not a stand-in for the real
   ROM's logic, only for its two addresses.

   Sequence:
     1. visit gate1 (0x1071C4, the real ROM's "tbz w0,#0") with w0 = 0x1111, sp = 0x70020000   (-> first hit)
     2. set x8 = 0x5555, x19 = 0x70030000 (the real ROM's "cbz w8" and the base [x19+0x7DA] would be read from)
     3. visit gate2 (0x107214, the real ROM's "cbz w8") with sp = 0x70030000                   (-> second/last hit)
     4. PSCI SYSTEM_RESET

   A passing run must report hits=2, first.{pc=0x1071c4,x0=0x1111,sp=0x70020000} (x1/x2/x3 stay 0, gate1's own
   capture only sets x0), last.{pc=0x107214,x0=0x5555,x1=0x70030000,sp=0x70030000} -- this proves the hook reads
   the RIGHT register at each address (w0 at the first, w8 and x19 at the second), not just that it fires twice. */
.text
.global _start
_start:
    mov   x0, #0x1111              // w0: what the real ROM's 0x10D0B8 would have returned, tbz-tested
    movz  x9, #0x0000
    movk  x9, #0x7002, lsl #16     // sp = 0x70020000
    mov   sp, x9
    b     gate1

.org 0x71c4
gate1:
    nop                            // probe fires here: captures w0
    mov   x8, #0x5555              // w8: the real ROM's guest-RAM status byte, cbz-tested
    movz  x19, #0x0000
    movk  x19, #0x7003, lsl #16    // x19 = 0x70030000 (the real ROM's [x19+0x7DA] base)
    movz  x9, #0x0000
    movk  x9, #0x7003, lsl #16     // sp = 0x70030000
    mov   sp, x9
    b     gate2

.org 0x7214
gate2:
    nop                            // probe fires here: captures w8 and x19
    movz  w0, #0x0009
    movk  w0, #0x8400, lsl #16     // w0 = 0x84000009 (PSCI SYSTEM_RESET)
    mov   x1, #0
    mov   x2, #0
    mov   x3, #0
    hvc   #0
forever:
    b     forever
