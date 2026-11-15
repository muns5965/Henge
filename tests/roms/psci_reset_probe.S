/* C1-2 (boot-diagnostic addition): the SYSTEM_RESET counterpart of tests/roms/psci_off_probe.S -- see that
   file's header for the full rationale, the diagnostic record this exercises, and the EL1/HVC assumptions.
   This one asks PSCI function 0x84000009 (SYSTEM_RESET) instead of 0x84000008 (SYSTEM_OFF), with x1=x2=x3=0,
   then loops. A passing run must report fn=0x84000009, "SYSTEM_RESET", cpu0, and a pc inside this file's own
   code -- and, paired with psci_off_probe.S's run, the two must never be reported with each other's function
   number or name (tools/c1vm-test.sh's psci-probe section checks both runs against each other, not just each
   one against its own expectation). */
.text
.global _start
_start:
    movz  w0, #0x0009
    movk  w0, #0x8400, lsl #16   // w0 = 0x84000009 (PSCI SYSTEM_RESET)
    mov   x1, #0
    mov   x2, #0
    mov   x3, #0
    hvc   #0
forever:
    b     forever
