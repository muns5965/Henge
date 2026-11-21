#!/bin/sh
# Checks that a short, explicit list of "contract values" shared between production code and a test file
# actually agree -- not merely that both files happened to be touched in the same commit (two files being in
# the same diff does not prove their values still match: 2026-11-21's panic-probe and USB-on-normal-boot
# regressions on research/android-port-g2b1 both landed with the production side changed and the test side's
# own hardcoded expectation left exactly as it was, in different commits, weeks apart).
#
# Two independent checks, both implemented as value comparisons (never a bare "was the file touched" check,
# since that proves nothing about whether the touch kept the numbers in sync):
#   1. panic-probe hook addresses: host/ui_runner_vm.c's HV_PANIC_PROBE_PC1/PC2 vs tests/roms/panic_probe.S's
#      two .org addresses (compared after subtracting VMAPPLE_FIRMWARE, since the ROM file's .org is an
#      offset and the hook's constants are VMAPPLE_FIRMWARE-relative guest addresses).
#   2. USB-on-normal-boot setting: host/ui_runner_vm.c's "cfg.usb_on_normal_boot = N;" vs every
#      "usb device port present: M (want M)" check string in tests/test_c1vm_proc.c (both the "present" and
#      "want" halves of each check string must equal N, not just equal each other).
#
# Exit 0 and "ok" lines when every checked contract agrees. Exit 1 and "MISMATCH ..."/"FAILED ..." lines for
# a genuine disagreement or for not being able to read one side at all (a renamed symbol, a reworded check
# string -- this script not understanding the new shape is itself worth a human look, so it fails rather than
# silently passing).
#
# Usage: tools/check-test-contracts.sh               -- just the two value checks above (fast, no git needed;
#                                                        safe to run locally before every commit, and as an
#                                                        early, cheap CI step before anything ARM64-only).
#        tools/check-test-contracts.sh <base> <head> -- also runs an ADVISORY (warn-only, never fails the
#                                                        script) check: did a production file that carries one
#                                                        of these contracts change between <base> and <head>
#                                                        with none of its paired test/doc files changing too?
#                                                        This catches the "forgot the test entirely" case the
#                                                        value checks above can't see before the mismatched
#                                                        commit exists; it is advisory because plenty of edits
#                                                        to these files (comments, unrelated nearby code) do
#                                                        not touch any contract value at all.
set -u
cd "$(dirname "$0")/.."
fail=0

echo "=== contract 1: panic-probe hook addresses (host/ui_runner_vm.c <-> tests/roms/panic_probe.S)"
PC1=$(grep -oE '#define HV_PANIC_PROBE_PC1 0x[0-9a-fA-F]+ull' host/ui_runner_vm.c | grep -oE '0x[0-9a-fA-F]+')
PC2=$(grep -oE '#define HV_PANIC_PROBE_PC2 0x[0-9a-fA-F]+ull' host/ui_runner_vm.c | grep -oE '0x[0-9a-fA-F]+')
ORG1=$(grep -oE '^\.org 0x[0-9a-fA-F]+' tests/roms/panic_probe.S | sed -n 1p | grep -oE '0x[0-9a-fA-F]+')
ORG2=$(grep -oE '^\.org 0x[0-9a-fA-F]+' tests/roms/panic_probe.S | sed -n 2p | grep -oE '0x[0-9a-fA-F]+')
if [ -z "$PC1" ] || [ -z "$PC2" ]; then
  echo "FAILED could not read HV_PANIC_PROBE_PC1/PC2 from host/ui_runner_vm.c (renamed or reworded?)"; fail=1
elif [ -z "$ORG1" ] || [ -z "$ORG2" ]; then
  echo "FAILED could not read two .org addresses from tests/roms/panic_probe.S (reworded?)"; fail=1
else
  # tests/roms/panic_probe.S's .org values are offsets into the raw ROM image; the hook's PC1/PC2 are
  # VMAPPLE_FIRMWARE(devices/vmapple.h, 0x100000)-relative guest addresses -- same number space once the base
  # is subtracted, since tools/build-asm-rom.sh loads the stripped image starting at that exact address.
  PC1_OFF=$(printf '0x%x' $((PC1 - 0x100000)))
  PC2_OFF=$(printf '0x%x' $((PC2 - 0x100000)))
  ORG1_N=$(printf '0x%x' $((ORG1)))
  ORG2_N=$(printf '0x%x' $((ORG2)))
  m=0
  [ "$PC1_OFF" = "$ORG1_N" ] || { echo "MISMATCH point A: hook HV_PANIC_PROBE_PC1=$PC1 (offset $PC1_OFF) but panic_probe.S's first .org is $ORG1_N"; m=1; }
  [ "$PC2_OFF" = "$ORG2_N" ] || { echo "MISMATCH point B: hook HV_PANIC_PROBE_PC2=$PC2 (offset $PC2_OFF) but panic_probe.S's second .org is $ORG2_N"; m=1; }
  if [ "$m" = 1 ]; then fail=1; else echo "ok  hook addresses ($PC1/$PC2) match panic_probe.S's .org offsets ($ORG1_N/$ORG2_N)"; fi
fi

echo "=== contract 2: USB-on-normal-boot setting (host/ui_runner_vm.c <-> tests/test_c1vm_proc.c)"
USB_VAL=$(grep -oE 'cfg\.usb_on_normal_boot = [0-9]+;' host/ui_runner_vm.c | grep -oE '[0-9]+')
TOTAL=$(grep -cE 'usb device port present: [0-9]+ \(want [0-9]+\)' tests/test_c1vm_proc.c)
if [ -z "$USB_VAL" ]; then
  echo "FAILED could not read cfg.usb_on_normal_boot's value from host/ui_runner_vm.c (reworded?)"; fail=1
elif [ "$TOTAL" -eq 0 ]; then
  echo "FAILED found no 'usb device port present: N (want N)' check string in tests/test_c1vm_proc.c (reworded?)"; fail=1
else
  MATCHING=$(grep -cE "usb device port present: $USB_VAL \\(want $USB_VAL\\)" tests/test_c1vm_proc.c)
  if [ "$MATCHING" -ne "$TOTAL" ]; then
    echo "MISMATCH $((TOTAL - MATCHING)) of $TOTAL 'usb device port present' check(s) in tests/test_c1vm_proc.c do not read 'present: $USB_VAL (want $USB_VAL)', but host/ui_runner_vm.c sets cfg.usb_on_normal_boot=$USB_VAL"
    fail=1
  else
    echo "ok  all $TOTAL 'usb device port present' check(s) in tests/test_c1vm_proc.c agree with cfg.usb_on_normal_boot=$USB_VAL"
  fi
fi

if [ $# -ge 2 ]; then
  base=$1 head=$2
  echo "=== advisory: files changed between $base and $head (warn-only; a warning here is not a failure)"
  CHANGED=$(git diff --name-only "$base" "$head" -- host/ui_runner_vm.c tests/roms/panic_probe.S tools/c1vm-test.sh tests/vm_fake.c tests/test_c1vm_proc.c docs/boot-diagnostic-contract-2026-11-15.md 2>/dev/null) || CHANGED=""
  has() { echo "$CHANGED" | grep -qx "$1"; }
  if has host/ui_runner_vm.c && ! has tests/roms/panic_probe.S && ! has tools/c1vm-test.sh; then
    echo "WARN host/ui_runner_vm.c changed but neither tests/roms/panic_probe.S nor tools/c1vm-test.sh did -- if this touched the panic-probe hook's addresses or captured registers, re-run contract 1 above by hand to be sure"
  fi
  if has host/ui_runner_vm.c && ! has tests/vm_fake.c && ! has tests/test_c1vm_proc.c; then
    echo "WARN host/ui_runner_vm.c changed but neither tests/vm_fake.c nor tests/test_c1vm_proc.c did -- if this touched cfg.usb_on_normal_boot or the USB port presence check, re-run contract 2 above by hand to be sure"
  fi
fi

exit $fail
