/* C1-2 (boot-diagnostic addition, docs/boot-diagnostic-contract-2026-11-15.md's stage-2 "known gap C-2"): the
   smallest possible synthetic firmware to exercise and verify core/exception.c's new psci_halt_* diagnostic
   record end to end. No RAM writes, no framebuffer, no disk/network access -- it does exactly one thing: ask
   the engine to power the machine off via PSCI SYSTEM_OFF (function 0x84000008), with x1=x2=x3=0, then sit in
   a loop in case that call somehow returns.

   A passing run of this ROM must report (host/ui_runner_vm.c's "ui_runner_vm RESULT:" line, or host/vmrun.c's
   "vmrun RESULT:" line -- both now carry this) fn=0x84000008, "SYSTEM_OFF", cpu0, and a pc inside this file's
   own code (VMAPPLE_FIRMWARE = 0x00100000, devices/vmapple.h) -- see tools/c1vm-test.sh's psci-probe section
   for the exact check, and tests/roms/psci_reset_probe.S for the SYSTEM_RESET counterpart this is paired
   against (the two must never be confused with each other's function number).

   The CPU starts at EL1 (same assumption tests/roms/frame_probe.S's header documents), so `hvc #0` reaches
   core/exception.c's PSCI handler directly: it reads the function number from x0 (core/exception.c, the HVC
   case: `cpu->x[0] = psci(cpu, cpu->x[0])`). clang's integrated assembler (tools/build-asm-rom.sh) does not
   expand a single `mov` pseudo-op across two nonzero 16-bit halves, so 0x84000008 needs movz+movk like
   frame_probe.S's multi-chunk immediates do. */
.text
.global _start
_start:
    movz  w0, #0x0008
    movk  w0, #0x8400, lsl #16   // w0 = 0x84000008 (PSCI SYSTEM_OFF)
    mov   x1, #0
    mov   x2, #0
    mov   x3, #0
    hvc   #0
forever:
    b     forever
