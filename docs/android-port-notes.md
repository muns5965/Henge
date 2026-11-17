# Android port — boot/device compatibility notes

A running log of what's confirmed, what's still a hypothesis, and what's been
retracted while debugging why a VMApple guest resets instead of completing
boot on the Android port. This is a verification ledger, not a narrative:
each entry should be updated in place as new evidence arrives, rather than
rewritten wholesale.

Scope note: this document covers Henge's own device emulation and host-side
boot path (all GPL, all in this repo). It deliberately does not include
instruction-level disassembly of the proprietary Apple boot ROM
(`AVPBooter.vmapple2.bin`) that Henge loads as guest firmware — findings
there are tracked separately. Every entry below is reproducible from this
repo's source and from VM run logs alone.

## Status legend

- **확인됨 (confirmed)** — verified directly against source or against a
  concrete, logged VM run.
- **구현 확인됨 (implementation confirmed)** — the code's current behavior is
  verified, but whether it matches real hardware/VZ is not.
- **미확인 (unconfirmed)** — a hypothesis consistent with the evidence so
  far, not yet proven.
- **철회됨 (retracted)** — an earlier claim in this investigation that
  turned out to be wrong or unsupported, kept here so it isn't re-proposed.

## 1. ROM loading and CPU init — 확인됨

Traced end to end through source:

- `android/app/.../MainActivity.java`'s `findLocalRom()`/`isGenuineRom()`
  checks the bundled ROM file's size and SHA-256 before use.
- `host/vmrun.c` takes `--rom`/`--disk`/`--aux` as plain file paths; no
  content is read at this stage.
- `host/vm.c`'s `hv_vm_create()` opens ROOT/AUX storage and builds the
  virtual hardware (`hv_vmapple_create()`) *before* the ROM file is read.
- `devices/vmapple.c`: `ROM_SIZE` is a fixed 1 MiB region mapped at
  `VMAPPLE_FIRMWARE = 0x00100000`, separate from main RAM (`0x70000000`).
  `hv_vmapple_load_rom()` is a plain `fread()` of up to 1 MiB into that
  region — no decompression, no parsing, and no hash check at this layer
  (the Android app's check above is the only integrity check in this path).
  The unused tail of the 1 MiB region reads as zero (anonymous mmap).
- `hv_vmapple_reset_cpu()` sets CPU0's PC to `VMAPPLE_FIRMWARE` (`0x100000`),
  EL1, MMU off; other CPUs start off, awaiting PSCI CPU_ON.

Conclusion: the loader does not place or transform the ROM in any way that
would itself explain a boot failure. Everything downstream of this is the
ROM's own code running.

## 2. CFG device (`devices/cfg.c`) — 확인됨

Exposes a `VMAppleCfg` struct (version, nr_cpus, ECID, ram_size,
`run_installer1`/`run_installer2`, MAC addresses, serial/model/soc_name
strings, etc.) as MMIO-backed memory. `run_installer1`/`run_installer2` are
the installer-vs-normal-boot switch read by the ROM; both are 0 for a
normal boot, 1/1 for an installer boot in the runs checked so far.

## 3. BDIF device (`devices/bdif.c`) — 확인됨 / 구현 확인됨

- Devices are selected by `off & DEVID_MASK`: `DEVID_ROOT=0x0000`,
  `DEVID_AUX=0x00010000`, `DEVID_USB=0x00100000`.
- For ROOT/AUX, `REG_NEXT_DEVICE` (`+0x420`) correctly reports capacity in
  512-byte sectors (`disk->size / 512`), and matches the capacity the ROM
  reads in logged runs (AUX: 65,536 sectors / `0x10000`, matching the AUX
  file's guest-visible size — see §4).
- Storage commands (`command()`) read/write the real ROOT/AUX file via
  `hv_disk_read`/`hv_disk_write`, logging
  `bdif %s: sector %u len %u flags %x -> %s` at `log>=2` or on failure.
- **USB slot fallback (구현 확인됨, compatibility candidate):** when no USB
  device is attached (`b->usb == NULL`), `bdif_read()`'s check
  `if (devid == DEVID_USB && b->usb) return usb_read(...)` is false, so a
  read of the USB slot's registers falls through to the *generic*
  switch — the same one ROOT/AUX use for unrecognized registers. Observed
  values for an absent USB slot:

  | register | value | meaning when a device *is* present |
  |---|---|---|
  | `+0x0` (`REG_STATUS`) | 1 ("active") | also 1 |
  | `+0x4` (`REG_CFG`) | 2 (generic placeholder) | `0x1A01` (`USB_DEVICE_TYPE`) |
  | `+0x420` (`REG_NEXT_DEVICE`) | `~0ull` | capacity / descriptor-dependent |

  So an absent USB slot currently reads back as "active" with a generic,
  non-USB config value, rather than as absent. **Whether this differs from
  what real Apple Virtualization.framework returns for an unpopulated slot
  is not yet established** — flagging as a candidate, not a confirmed bug.

## 4. AUX storage format — 확인됨

- `host/vm.c`: `hv_disk_open(&aux, c->aux, c->aux_offset ? c->aux_offset : 16384, ...)`
  — the first 16 KiB of the AUX file is host-side metadata, invisible to
  the guest; the guest's "sector 0" is file offset 16384.
- `host/disk.h` documents this as matching QEMU's vmapple convention.
- `host/vmrun.c`'s `make_aux()` writes a fixed 24-byte magic header at
  offset 0, then `ftruncate()`s to 33,570,816 bytes. Guest-visible capacity
  is therefore `33,570,816 - 16,384 = 33,554,432` bytes = 65,536 sectors —
  matches the `REG_NEXT_DEVICE` value logged for AUX (§3).
- Everything from byte 24 onward (including the entire guest-visible
  region) is zero-filled by `ftruncate()`, since no personalization/NVRAM
  data is written into a freshly created AUX file.
- **iOS parity — 확인됨:** `ios/Henge/Native/henge-vm.c`'s
  `henge_make_aux()` is byte-for-byte identical (same 24-byte header
  content, same `ftruncate(33570816)`), called from
  `ios/Henge/Installer.swift:190`. The blank-AUX behavior is inherited from
  the existing iOS implementation, not introduced by the Android port.

### 철회됨: "빈 AUX가 개인화 검증 실패를 유발한다"

An earlier pass in this investigation concluded that a blank/zero AUX file
would naturally fail guest-side personalization checks (reasoning from a
`SNON`/nonce-shaped SVC call sequence seen in ROM execution logs) and that
this explained the observed reset. **This causal chain is not established**
and should not be treated as a conclusion:
- The host's `SVC #0` handling (`core/exception.c`) only forwards the
  instruction to the guest's own EL1 exception vector
  (`hv_take_exception(..., HV_EC_SVC64 << 26 | ..., pc+4)`); Henge's host
  code does not interpret or act on the SVC's purpose, so "a personalization
  SVC was seen" cannot by itself mean "it failed".
  - What that SVC's guest handler actually does with it is unconfirmed (see
    the (not yet published) ROM-internal notes for the control-flow
    candidates traced so far).
- Whether a blank AUX is in fact invalid input for a *new, not-yet-installed*
  VM (vs. only for an already-installed one doing a normal boot) is unknown.
  iOS using the identical blank-AUX format for new VMs is evidence against
  "blank AUX is always rejected", but is not proof either way for a normal
  (post-install) boot specifically.

## 5. Boot-mode experiments — 확인됨 (per-run) / 미확인 (root cause)

- `devices/vmapple.c:336-338`:
  ```c
  /* Installer boots (the ROM goes to DFU) get the USB device port, as VZ gives it.
     HV_USB_ON_NORMAL is an experiment: the same port on a normal boot
     (run_installer1/2 stay 0), to see whether the ROM's USB slot check, which
     a normal boot fails with the slot absent, is what ends it. Off by default. */
  if (cfg->run_installer1 || cfg->run_installer2 || getenv("HV_USB_ON_NORMAL")) {
  ```
  Real VZ gives the USB device port only on installer boots. Henge's
  `HV_USB_ON_NORMAL` env var is an explicit, off-by-default experiment to add
  that port to a normal boot anyway, for comparison. It is **not** a
  standard boot configuration.
- Build 144 ("정상 부팅 + USB 프로브" run, `vmrun_normal_usb_probe_log3_*`)
  was run with `HV_USB_ON_NORMAL` set — i.e. a non-standard
  normal-boot-with-USB configuration, not a plain normal boot.
- Two separate **plain** normal-boot runs (no USB port, `run_installer=0`,
  ECID/seed 0, fresh blank AUX), dated 2026-11-14 and 2026-11-16, **both
  also hit `PSCI SYSTEM_RESET`**, ending near PC `0x104518`/`0x104514`/
  `0x104504`/`0x1044F0` across the two runs (PCs differ slightly between
  runs; not yet shown to be the same internal branch).
  - **Conclusion: a normal boot resets even with no USB port at all.**
    This rules out "USB artificially present" as *the* explanation for the
    reset, though USB presence clearly does change the ROM's execution path
    (confirmed difference in BDIF register values read, §3).
- **미확인:** whether the USB-slot fallback behavior in §3 is what a plain
  normal boot's reset actually hinges on. Not yet shown: that the ROM reads
  the USB slot's registers at all during a plain normal boot, or what it
  does with the fallback value `2` if it does.

## 6. Open questions / next verification steps

1. On a plain normal boot (no `HV_USB_ON_NORMAL`), does the ROM actually
   touch `DEVID_USB` registers before resetting? (Needs a diagnostic run
   with detailed MMIO logging — `HV_DIAG_PROBE_CALLER` / `--log 3` — on a
   plain normal boot; existing logs don't have this detail.)
2. If it does, what does it do with the fallback value it reads (`REG_STATUS=1`,
   `REG_CFG=2`)? Does it branch on "device present but wrong type", or
   "device present, type `2` happens to be acceptable", or ignore it
   entirely?
3. What should an *absent* BDIF slot report, to match real
   Virtualization.framework behavior for an unpopulated device? (Not yet
   researched; "set REG_STATUS/REG_CFG to 0" is a guess without a basis to
   justify it yet.)
4. Confirm whether the two plain-normal-boot runs (11-14, 11-16) reach the
   reset via the same internal branch, given their differing final PCs.

No disk/ROM/source changes have been made as part of this investigation;
this document only records analysis of existing source and existing VM run
logs.
