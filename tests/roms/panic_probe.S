/* [C1-4] Exercises host/ui_runner_vm.c's Android panic-entry probe end to end: panic_probe_install()'s two
   stop_pc addresses (0x105F2C / 0x105F9C -- the two generic-panic-helper entry points this investigation found
   by ROM disassembly in the real AVPBooter ROM, see docs/boot-diagnostic-contract-2026-11-15.md), core/cpu.h's
   HvCpu.panic_first/panic_last/panic_hits capture in panic_probe_hook(), core/exception.c's psci() copying them
   into the machine-wide snapshot, and host/vm.c's hv_vm_panic_probe() accessor.

   This ROM is built for VMAPPLE_FIRMWARE-relative addressing (tools/build-asm-rom.sh strips to a raw .text image
   loaded starting at devices/vmapple.h's VMAPPLE_FIRMWARE = 0x100000), so .org below places code at the EXACT
   guest addresses the real ROM's two probe points sit at -- this file is not a stand-in for the real ROM's logic,
   only for its two addresses.

   Sequence (matches tools/c1vm-test.sh's "T3" check -- probe A, then B, then A again, then SYSTEM_RESET):
     1. visit A (0x105F2C) with x0..x3 = 0x1111/0x2222/0x3333/0x4444, sp = 0x70020000   (-> first hit)
     2. fall through to a B setup block with x0..x3 = 0x5555/0x6666/0x7777/0x8888, sp = 0x70030000
     3. visit B (0x105F9C)                                                            (-> second hit)
     4. a second A setup block with x0..x3 = 0x9999/0xaaaa/0xbbbb/0xcccc, sp = 0x70040000
     5. visit A again (same address, different registers)                            (-> third and last hit)
     6. PSCI SYSTEM_RESET

   A passing run must report hits=3, first.{pc=0x105f2c,x0=0x1111,x1=0x2222,x2=0x3333,x3=0x4444,sp=0x70020000},
   last.{pc=0x105f2c,x0=0x9999,x1=0xaaaa,x2=0xbbbb,x3=0xcccc,sp=0x70040000} -- first and last share a PC (both
   are the A address) but differ in every register, which is exactly what proves the hook captures the FIRST
   hit without letting a later one overwrite it, while still keeping the true last one. A register not in this
   probe's own list (x28) is used purely as this ROM's own visit counter, so panic_probe_hook never needs to
   know or care about it. */
.text
.global _start
_start:
    mov   x28, #0                 // this ROM's own counter: 0 before A has been visited once, nonzero after

    mov   x0, #0x1111
    mov   x1, #0x2222
    mov   x2, #0x3333
    mov   x3, #0x4444
    movz  x9, #0x0000
    movk  x9, #0x7002, lsl #16     // sp = 0x70020000
    mov   sp, x9
    b     probe_a

.org 0x5f2c
probe_a:
    nop
    cmp   x28, #0
    b.eq  to_b_setup
    b     to_reset

to_b_setup:
    add   x28, x28, #1
    mov   x0, #0x5555
    mov   x1, #0x6666
    mov   x2, #0x7777
    mov   x3, #0x8888
    movz  x9, #0x0000
    movk  x9, #0x7003, lsl #16     // sp = 0x70030000
    mov   sp, x9
    b     probe_b

.org 0x5f9c
probe_b:
    nop
    mov   x0, #0x9999
    mov   x1, #0xaaaa
    mov   x2, #0xbbbb
    mov   x3, #0xcccc
    movz  x9, #0x0000
    movk  x9, #0x7004, lsl #16     // sp = 0x70040000
    mov   sp, x9
    b     probe_a

to_reset:
    movz  w0, #0x0009
    movk  w0, #0x8400, lsl #16    // w0 = 0x84000009 (PSCI SYSTEM_RESET)
    mov   x1, #0
    mov   x2, #0
    mov   x3, #0
    hvc   #0
forever:
    b     forever
