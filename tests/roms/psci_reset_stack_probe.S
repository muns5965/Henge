/* C1-2 (boot-diagnostic addition): exercises core/exception.c's psci_halt_sp / psci_halt_x26 /
   psci_halt_stack[] capture end to end (added for the Android-port investigation into WHICH of several ROM
   call sites reaches the real vmapple firmware's own PSCI SYSTEM_RESET call -- see the stack-frame analysis
   this was added to confirm). Unlike psci_reset_probe.S, this one gives the engine something nontrivial to
   capture: it moves SP into guest RAM, writes four distinct marker values at four distinct byte offsets
   past SP (8, 0x38, 0x48 and 0x78 -- the ones this investigation's stack-frame analysis has cared about so
   far, across two rounds of widening psci_halt_stack[]; any of the HV_PSCI_HALT_STACK_WORDS words
   host/vm.h's hv_vm_psci_halt_stack hands back are fair game to check), sets x26 to a fifth marker, then
   calls PSCI SYSTEM_RESET.

   A passing run must report sp=0x70010000, x26 ending in ...0026, and the stack words at index 1 (sp+8),
   index 7 (sp+0x38), index 9 (sp+0x48) and index 15 (sp+0x78) ending in ...0019, ...0038, ...0048 and
   ...0078 respectively -- see tools/c1vm-test.sh's check. This does NOT replicate the real ROM's call
   chain (0x105FC0 -> 0x103db0 -> 0x104530 -> the shared PSCI wrapper); it only proves the capture mechanism
   itself reads back exactly what was in memory/registers at the hvc, with no off-by-one or sign-extension
   mistake in core/exception.c's psci_halt_peek_u64 or in how host/vm.c's hv_vm_psci_halt_stack and the two
   RESULT-line formatters hand the values onward, at the current, widened word count. */
.text
.global _start
_start:
    movz  x26, #0x0026
    movk  x26, #0xaaaa, lsl #48   // x26 = 0xaaaa000000000026 (never touched again: proves the live-register path)

    mov   x0, #0x70010000         // well inside guest RAM (VMAPPLE_MEM = 0x70000000)
    mov   sp, x0

    movz  x2, #0x0019
    movk  x2, #0xbeef, lsl #48    // x2 = 0xbeef000000000019
    str   x2, [sp, #8]            // the "original w19" offset this investigation's stack analysis names

    movz  x3, #0x0038
    movk  x3, #0xcafe, lsl #48    // x3 = 0xcafe000000000038
    str   x3, [sp, #0x38]         // the "outer caller's saved x30" offset this investigation's stack analysis names

    movz  x4, #0x0048
    movk  x4, #0xdead, lsl #48    // x4 = 0xdead000000000048
    str   x4, [sp, #0x48]         // one call-frame further up (0x105FC0's own saved x30), added in the 16-word widening

    movz  x5, #0x0078
    movk  x5, #0xf00d, lsl #48    // x5 = 0xf00d000000000078
    str   x5, [sp, #0x78]         // the new ceiling (index 15) added in the same widening, for headroom beyond it

    movz  w0, #0x0009
    movk  w0, #0x8400, lsl #16    // w0 = 0x84000009 (PSCI SYSTEM_RESET)
    mov   x1, #0
    mov   x2, #0
    mov   x3, #0
    hvc   #0
forever:
    b     forever
