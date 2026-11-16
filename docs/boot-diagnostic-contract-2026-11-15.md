# C-track: boot diagnostic contract

Status: written ahead of any actual ROM boot attempt (originally alongside no code change), per the review
chain's repeated point (most recently documents `c27c6103` and `fcb40083`-adjacent follow-ups): when the
Apple logo does not appear, "it didn't boot" is not an answer by itself — the contract below is what lets a
later attempt say *which* of several independent stages failed, instead of guessing from one symptom.

`START_ENABLED` is now `true` (flipped after this document was written, with its own review/approval) and a
first real-device attempt has already happened — see stage 2's update below for the one piece of code this
contract's own "known gap C-2" led to. This document's five stages are still the frame to read any attempt's
logs through; only that one gap's status has moved since the line above was first written.

## The rule that matters most

**A synthetic frame must never be read as a real guest frame.** `DisplayTestActivity` (C0+B0) and
`VmDisplayActivity` (A) share one native display pipeline (`android/app/src/main/cpp/ui_jni.c`), but they are
never the two sources of a single judgment: a screen full of pixels only means "boot succeeded" when it came
through `VmDisplayActivity`'s runner-mode path, fed by `host/ui_runner_vm.c`'s real `hv_vm_boot_display()` —
never by `DisplayTestActivity`'s `hv_testpattern()` synthetic picture, and never by a leftover shared-memory
file from an earlier, different run (the pid check in `nativeAttachRunner` exists for exactly this reason:
host/frame_shm's stats carry `runner_pid`, and the Activity verifies it before trusting the file at all).

**Process start, guest boot, and the Apple logo are three separate questions.** A "yes" on one is not evidence
for the others:
- The VM runner process starting and reaching `HV_SHM_RUNNING` says the engine initialized a machine object.
  It says nothing about whether the guest CPU is making progress.
- The guest CPU running and not halting/faulting says nothing about whether it has drawn anything yet (most
  of early boot has no framebuffer at all).
- A frame existing (PVG-drawn or `boot_args`-scanned) and reaching Android's `SurfaceView` says nothing about
  what that frame's *content* is — it could be a boot-progress pattern, a blank/garbage buffer the scan found
  by coincidence, or (further along) the actual Apple logo. Confirming it is specifically the Apple logo is a
  visual check (a saved frame, or the person looking at the screen), not a counter.

## The five stages

Each stage names: what it asks, the concrete signal to read, how to read it today, and what a failure there
does (and does not) tell you about the stages after it.

### 1. VM initialization

**Asks**: did `host/ui_runner_vm.c` build a machine object at all, with the settings this port is willing to
run?

**Signal**: the process's own stdout, read today by `VmDisplayActivity`'s output-reading thread into
`runnerLine` (and the last 40 lines into `runnerLog`, not currently surfaced to the screen):
- `"ui_runner_vm: started pid %d, virtual machine mode"` — the runner exists.
- `"ui_runner_vm: the settings were refused: %s"` → exit code 1. Stop here; nothing below is reached.
- `"ui_runner_vm: the machine could not be made: %s"` → exit code 1. Stop here.
- `"ui_runner_vm: the machine has a USB device port; it is not started"` → exit code 1, a dedicated safety
  refusal (`host/ui_runner_vm.c`, the line right after machine creation: `"usb device port present: %d (must
  be 0)"` — a nonzero value here is itself the failure, independent of anything else). Stop here.
- `"ui_runner_vm: machine created in %.0f ms; usb device port present: 0"` — this stage passed.

**A failure here means none of stages 2-5 can be evaluated** — there is no running machine for them to be
about. It does not by itself say anything about the ROM's own validity beyond what the refusal message names.

### 2. Guest CPU progress

**Asks**: once the machine exists, is the guest CPU actually executing, or stuck/exited immediately?

**Signal**: `host/ui_runner_vm.c`'s own run loop and its final line, `"ui_runner_vm RESULT: ..."`, plus
`hv_vm_reason_name()` (`host/vm.c`) for why a run ended: `"halt"`, `"wfi with nothing to wake it"`,
`"unsupported"`, `"fault on the exception vector"`, or `"other"`. A run that exits immediately with one of
these reasons, before stage 3 ever reports a present frame, places the failure in CPU execution, not in
display or shared-memory plumbing.

**Caution carried over from earlier in this session**: `core/machine.c`'s `reset_why[]` counters are a JIT
**code-cache invalidation** tally (TLBI, ASID change, code-modification detection) — unrelated to guest
boot/DFU resets. Do not read a nonzero `reset_why` entry as evidence about boot progress; it was already
investigated and retracted once as a hypothesis for a different question, and the same mistake is possible
here.

**Known gap (C-2, flagged in review, PARTIALLY closed for the PSCI case)**: `hv_vm_reason_name()` still gives
only a coarse category (halt / wfi-with-nothing-to-wake-it / unsupported / fault-on-the-exception-vector /
other) for why a run ended, and for a fault or an unsupported instruction it is still not a program counter —
that part of the gap is unchanged. For a `"halt"` caused specifically by the guest's own PSCI call
(`SYSTEM_OFF`/`SYSTEM_RESET`, `core/exception.c`'s `psci()`), the RESULT line now also carries which of the
two it was, the calling CPU's index, and the PC/LR captured AT the `hvc` instruction itself (`core/machine.h`'s
`psci_halt_*` fields, read through `host/vm.c`'s `hv_vm_psci_halt_info()`; `host/ui_runner_vm.c`'s and
`host/vmrun.c`'s RESULT lines both carry it, independent of `HV_PSCI_LOG`/any `HV_*` variable). A `"halt"` with
no `psci ...` clause in the RESULT line means the machine stopped some other way (`hv_vm_stop`, a test's own
`stop_pc`) that this record does not cover. This still does not say which ROM FUNCTION or boot stage that PC
belongs to — matching an observed PC against ROM disassembly/address analysis is, as before, a separate step
for whoever holds that analysis, not something this field does automatically — and the fault/unsupported
categories still have no PC captured at all; only the PSCI-halt path got this.

A second accessor added alongside the first, for the SAME question this PC/LR pair could not answer by
itself: a ROM can reach one shared PSCI-call wrapper from several call sites, and `psci_halt_lr` is that
wrapper's OWN return address one level up (into whichever helper it calls just before the `hvc`), not the
address of whichever outer caller reached the wrapper in the first place. `host/vm.c`'s
`hv_vm_psci_halt_stack()` hands back the halting CPU's SP and x26 at the `hvc` (live-register reads) plus a
raw, uninterpreted copy of guest memory `[sp, sp+HV_PSCI_HALT_STACK_WORDS*8)` (`core/exception.c`'s
`psci_halt_peek_u64`, which never raises a guest fault or touches an MMIO device to get it — a word it
cannot translate to plain guest RAM is just reported as missing, not guessed). `HV_PSCI_HALT_STACK_WORDS`
(`core/machine.h`) started at 8 words (`[sp, sp+0x40)`) and was widened once to 16 words (`[sp, sp+0x80)`)
mid-investigation, once a confirmed real-device call chain turned out to have one more stack frame between
the halting wrapper and the ROM's own outer caller than the 8-word window could see (the frame needed was at
`sp+0x48`, past the original `sp+0x3f` ceiling) — the window can be widened again the same way if a future
call chain needs to reach further still. Which byte offset in that window holds something useful (an outer
caller's own saved return address, a caller's preserved register) is a property of how the SPECIFIC ROM
build under investigation lays out its stack frames at that point — this accessor does not compute or assume
that, the same way the ROM-hex-dump workflow elsewhere in this investigation does not interpret bytes before
they reach someone who has worked out the surrounding function's structure.

### 3. Guest frame existence

**Asks**: has the guest produced anything `hv_vm_boot_display()` (`host/vm.c`) can call a frame yet?

**Signal**: `HvRunnerStats.display_present` / `display_absent` (`host/runner_vm_core.h`, filled by
`fetch_cb()` in `host/runner_vm_core.c`), visible today via the `"status"` stdin command's printed line
(`host/ui_runner_vm.c`'s `status_line()`): `"display present %llu absent %llu"`. `hv_vm_boot_display()`
(`host/vm.c`) tries two sources, in order, before returning present/absent to this counter:
- A PVG-drawn frame (`hv_pvg_lite_frame()`) — present once the GPU ring protocol (this session's B-track) has
  actually produced output.
- Failing that, a `boot_args` scan of guest RAM (rate-limited to once every 2 s; see `host/vm.c`'s
  `last_scan` guard) — present once iBoot has written its early framebuffer descriptor. A `display_absent`
  count that is merely "still climbing" in the first couple of seconds of a cold boot is not yet a failure —
  the 2 s scan limit means an early poll can legitimately find nothing before the next scan runs.

**Known gap (C-1, flagged in review, not fixed here — this is a documentation limitation, not a feature that
exists today)**: `display_present`/`display_absent` is a single present-or-not counter. `hv_vm_boot_display()`
does not report back, and `fetch_cb()` does not record, WHICH of the two sources above actually produced a
present frame — a `display_present` count climbing is consistent with either a PVG frame or a `boot_args`-
found one, and this stage's signal cannot tell them apart. If that distinction turns out to matter during an
actual boot trial (e.g., to know whether the GPU ring path is contributing anything yet, versus the guest
only ever being seen through the early `boot_args` framebuffer), `hv_vm_boot_display()` would need to expose
which path it took as a separate output field — nothing today does this, and this document does not claim
otherwise.

**A nonzero `call_errors`** at this stage (same stats struct) is a THIRD outcome, distinct from both present
and absent: `fetch_cb()`'s own bounds check rejected what `hv_vm_boot_display()` returned (bad dimensions,
stride, or a size that would not fit `HV_SHM_PIXEL_BYTES`). That is a guest-or-engine-side data problem, not
"nothing to show yet" — it should not be folded into "absent" when reading the status line.

### 4. Shared-memory publication

**Asks**: once a frame exists, did it actually reach the shared file Android reads from?

**Signal**: `HvRunnerStats.frames_published` / `shm_errors` (same status line: `"frames published %llu"`,
`"call_errors %llu shm_errors %llu"`), cross-checked against the Android-side view of the SAME file:
`VmDisplayActivity`'s `impl_status_vm()` (`ui_jni.c`) reports `HvShmStats.frames` (there called `published`)
and `rejected`, read independently through `hv_shm_stats()` from the consumer side. The two sides' publish
counts should track each other (within the Android side's own poll cadence); a persistent gap between the
runner's `frames_published` and the Android side's `published` count localizes the problem to the shared-file
handoff itself (a lock that is not being taken, `shm_errors` growing, or `HvShmStats.recovered` climbing,
meaning the file's owner lock had to be recovered from a dead holder) rather than to frame generation (stage
3) or rendering (stage 5).

### 5. Android reception and rendering

**Asks**: once a frame is in the shared file, does it reach the screen?

**Signal**: `VmDisplayActivity`'s own native status (`impl_status_vm()`): the `pointer ... (known: ...)` line
confirms the shared file is attached and `hv_shm_get_pointer()` is returning data at all; `HvDispStats` (the
`frames`/`shown`/`rejected`/`closed_refused` counts `impl_status_vm()` prints first) comes from
`hv_dispbuf_stats()` on the Android side's OWN `display_buf` instance (`fetch_shm()` in `ui_jni.c`, pulling
through `hv_shm_read_frame()`) — a count that advances only once a frame is actually blitted into the
`SurfaceView`'s buffer (`ANativeWindow_lock`/`_unlockAndPost` in `impl_nativeRender()`). `frames` advancing
while `shown` (the `blits` field) does not means frames are being pulled from shared memory but never making
it onto the screen (a window/surface problem, not a shared-memory or guest problem) — the existing
`__android_log_print(ANDROID_LOG_WARN, ...)` for a non-RGBX/RGBA window format is the concrete place that
distinguishes this from a silently-absent surface.

**Confirming the Apple logo specifically** is outside what any counter above can say: it is a one-time visual
check against a captured frame (or the screen itself), done only after stages 1-4 all show a healthy, real
(not synthetic) frame reaching this stage.

## What this contract does not cover

- The ROM's own configuration and USB-initialization path a separate analysis apparently already covered
  ("어제 분석한 ROM의 설정 및 USB 초기화 경로", per the review document that asked for this contract) is not
  reproduced here — that analysis was not available to this session to read and cross-reference accurately,
  so this document only names the one concrete, already-in-code USB safety check this session could verify
  directly (stage 1's "usb device port present" refusal). Whoever holds that ROM/USB analysis should cross-
  reference it against stage 1 and 2 above rather than this document inventing a connection it cannot verify.
- Signing/restore-related questions are out of scope here as elsewhere in this session (held items): this
  contract is about observing a boot attempt, not about what ROM/aux content is valid to attempt it with.

## Android panic-entry probe (added once the boot trial above reached a PSCI SYSTEM_RESET)

The first real boot trial reached stage 2's halt via a PSCI SYSTEM_RESET, not the ROM reaching further stages.
Static disassembly (ROM reads taken through the app's own read-only "ROM 구간 읽기"/"직접 분기 참조 검색"
features) plus the psci_halt_sp/x26/stack[] capture above traced the call chain all the way from the `hvc`
instruction back through `0x1044d4` (the shared PSCI wrapper) → `0x104530` (`do_reset`) → `0x103db0` → `0x105FC0`
→ confirmed, via the real device's own captured stack word at `sp+0x48`, to be reached from `0x106270`. From
there, ROM string analysis found two generic panic/assert helpers (an empty-message one at `0x105F2C`, a
`"%llx:%d"`-formatted one at `0x105F9C`) that `0x106060` -- the function `0x106270` calls into -- reaches with
`w2=1` specifically through one of these two helpers, which is what actually triggers the `SYSTEM_RESET`
(as opposed to two OTHER static callers of the same `0x106060`, found by branch-reference search, that pass
`w2=0` and only log). Each of these two helpers has tens of static callers (88 and 75 respectively) throughout
the ROM, corresponding to the many places a generic `assert()`-style check can fail, so which ONE actually fired
on this device could not be narrowed further by static analysis or stack capture alone -- the registers that
would say so (the helper's own entry `x0`-`x3`, and especially its `LR`, which is the one piece of information
that distinguishes among the ~163 static candidates) are long gone by the time execution reaches the `hvc`
several frames later.

`host/ui_runner_vm.c`'s `panic_probe_install()`/`panic_probe_hook()` close that gap: using the engine's existing
two `stop_pc`/`stop_pc2` slots (the same mechanism `host/vmrun.c`'s `HV_DIAG_PROBE`/`HV_DIAG_PROBE_CALLER` already
use, just a different pair of addresses and a host binary of its own), it records the first and the last time the
cpu that eventually makes the PSCI call hits either `0x105F2C` or `0x105F9C`, together with that cpu's `PC`,
`LR`, `X0`-`X3` and `SP` at the hit (`core/cpu.h`'s `HvPanicHit`/`HvCpu.panic_first`/`panic_last`/`panic_hits`).
First and last are kept separately, not just the latest, because a panic path can recurse through further
asserts before the final reset, and the FIRST entry -- the one whose `LR` actually names the real failing call
site among the ~163 candidates -- is the one this investigation needs, not whichever one happened last.

Unlike `vmrun.c`'s probes, this cannot be switched on by an `HV_*` environment variable: `ui_runner_vm.c` calls
`hv_runner_scrub_env()` before `hv_vm_create()` specifically so a debug-only variable can never quietly affect a
real boot through this binary (confirmed directly in source during this round of review, not assumed), so the
probe installs unconditionally in code instead, right after `hv_vm_create()` succeeds and before `hv_vm_start()`.
Installing it is read-only and harmless even against a different ROM than the one this investigation is about:
the hook only ever copies registers into that cpu's own `panic_first`/`panic_last`/`panic_hits` fields, never
touches guest-visible state, and a ROM that does not happen to execute at these two addresses simply never
triggers it.

The result is published through the exact same `psci_halt_valid` three-state RELEASE/ACQUIRE record as
`psci_halt_sp`/`x26`/`stack[]` above (`core/exception.c`'s `psci()` copies `panic_first`/`panic_last`/`panic_hits`
in the same compare-and-swap block, before the same final RELEASE store), read back through a third accessor,
`host/vm.h`'s `hv_vm_panic_probe()`, and printed as its own `"PANIC_DIAG"` RESULT line (kept separate from the
PSCI line's `reason_txt` rather than appended to it, since that buffer is already sized for the stack dump).
**Known, accepted limitation**: because this rides on the PSCI-halt publish, a panic that is somehow recovered
from, or a stop that is not a PSCI call at all (a fault, `hv_vm_stop`, a test's own `stop_pc` used for something
else), never publishes a `PANIC_DIAG` line, even if the probe did record hits on some cpu. Within a PSCI halt,
`hits=0` is printed plainly rather than the line being omitted: "neither probe address was reached" is itself a
real, reportable answer to the question this probe exists to ask, not a sign the probe failed to install or run.

Covered by `tests/roms/panic_probe.S` and `tools/c1vm-test.sh`'s "panic probe" check (this is the earlier, now
superseded version of that probe/ROM -- see the next section): a synthetic ROM visited `0x105F2C` (A), fell
through to `0x105F9C` (B), returned to `0x105F2C` (A again) with different registers each time, then called
`SYSTEM_RESET` -- a passing run reported `hits=3` with `first` matching the very first visit to A and `last`
matching the second, later one, proving the hook keeps the true first entry distinct from the true last even
though both happen to share the same `PC`.

### Round 2: the real device confirmed `0x105F2C`, so the probe moved one step further upstream

A real-device run on build 140 (Galaxy S26 Ultra, commit `a750848`) reported `hits=1`,
`first=last={pc=0x105F2C, lr=0x10723C, x0=0, x1=0x70028280, x2=0x70028550, x3=0x12B1A0, sp=0x7004ED50}`: of the
two helpers, only `0x105F2C` is ever reached, and its `LR` names the exact static call site -- `0x107238`'s
`bl 0x105F2C` -- among the ~163 candidates. Disassembling `0x105F2C` itself (via ROM reads of `0x105F20`) showed
it hard-codes `x0=0, x1=0` before calling `0x105F78`, so the panic message it produces is always the ROM's
default/empty one, and the caller's own `x1`-`x3` (whatever they were) are never used as message content --
an earlier read of this investigation's own working theory (that `x3=0x12B1A0` might be a format-string pointer)
was wrong and is corrected here; `0x12B1A0` is a small binary table/descriptor, not a C string.

Disassembling the ROM immediately around `0x107238` (reads of `0x107180`-`0x1072F0`) found two conditions
immediately upstream of that call, both confirmed by independent byte-level decoding of their encodings:
```
0x1071B8: adrp x19, 0x7002A000        (x19's only origin in this window -- a guest RAM page, not ROM)
0x1071C0: bl   0x10D0B8
0x1071C4: tbz  w0, #0, 0x107210        (w0 is 0x10D0B8's own return value)
0x107210: ldrb w8, [x19, #0x7DA]
0x107214: cbz  w8, 0x107238            (w8 was just loaded from guest RAM address 0x7002A7DA)
0x107238: bl   0x105F2C                (confirmed above to be the real device's own call)
```
**Important caution, raised and accepted during `/검토`**: the two addresses above are statically confirmed to be
the ROM's own code and to reach `0x107238` if taken, but which branch direction the real device actually took at
`0x1071C4`/`0x107214` on build 140 has NOT been measured -- only the resulting call to `0x105F2C` has. Static
structure is not a substitute for measuring the actual execution.

So the probe was retargeted (not duplicated: the engine has only one `stop_pc`/`stop_pc2`/`stop_hook` slot, so
only one probe can be installed per build -- switching which two addresses it watches is a source edit, not a
runtime option) from the now-confirmed `0x105F2C`/`0x105F9C` to these two upstream conditions, to measure, rather
than infer, whether the real device takes them and with what live values:
  - `0x1071C4`: `panic_first`/`panic_last`'s `.x0` holds `w0` (the `tbz`'s own tested register)
  - `0x107214`: `.x0` holds `w8` and `.x1` holds `x19` (the `cbz`'s tested register and the base `[x19+0x7DA]`
    was loaded from)
`core/cpu.h`'s `HvPanicHit` fields are reused generically for this, exactly as that struct's own comment already
allows ("which field means what... is specific to the ROM build under investigation, not computed or assumed
here"). Covered by the current `tests/roms/panic_probe.S` and `tools/c1vm-test.sh`'s "panic probe" check: a
synthetic ROM visits `0x1071C4` with `w0=0x1111`, then `0x107214` with `w8=0x5555, x19=0x70030000`, then resets --
a passing run must report `hits=2`, `first` matching the `0x1071C4` visit (only `.x0` set) and `last` matching the
`0x107214` visit (`.x0`/`.x1` both set), proving the hook reads the right register(s) at each address.

`0x7002A7DA` is guest RAM, not ROM, so it is outside what "ROM 구간 읽기"/"직접 분기 참조 검색" can read; it is
also not `devices/cfg.c`'s `VMAPPLE_CONFIG` region (that is a separate, fixed MMIO window at PA `0x00400000`, not
RAM-relative), so Henge's own existing device/config code is not a confirmed source of this byte either way --
that remains open. Tracing who is (or should be) writing to `0x7002A7DA`, and whether `0x10D0B8`'s return value
matters too, is follow-up work gated on what this round's measurement actually shows.

### Round 3: a real device confirmed both branches are taken, so the probe moved inside `0x10D0B8` itself

A real-device run on build 142 (commit `4b679eb`) reported `hits=2`,
`first={pc=0x1071c4, x0=0, x1=0, x2=0, x3=0}`, `last={pc=0x107214, x0=0, x1=0x7002a000, x2=0, x3=0}`: both
branches are actually taken, with `x19=0x7002a000` live and matching the static disassembly exactly -- this
confirms `0x7002A7DA == 0` is really read on the path to the panic, not merely a static possibility.

Disassembling `0x10D0B8` itself (ROM reads spanning `0x10D0B8`-`0x10D4A8`, cross-checked instruction-by-instruction
against the raw bytes, correcting two of this investigation's own earlier misreads along the way -- see below)
found it has at least two distinct ways to return 0 to its caller at `0x1071C0`:
```
0x10D14C: bl   0x10C51C
0x10D150: cbz  w0, 0x10D184          (w0 is 0x10C51C's return value)
; w0 != 0 (0x10C51C failed):
0x10D154: mov  w8, #0xDD             (an error-report call follows, then:)
0x10D17C: mov  w19, #0
0x10D180: b    0x10D2CC              -> early-failure return, x0=0
; w0 == 0 (0x10C51C succeeded): falls through to 0x10D184, more setup, eventually:
0x10D184: adrp x20, 0x7002B000
0x10D188: add  x20, x20, #0x470      (x20 = 0x7002B470, a 3-byte status table this function itself maintains:
                                       0x10D190/0x10D194 write 0 and the function's own argument to +1/+0)
  ...
0x10D2A4: bl   0x104E04
0x10D2B8: cbz  x0, 0x10D2E0          (x0 is 0x104E04's return value -- note: tests the FULL 64-bit register)
; x0 != 0 (0x104E04 succeeded):
0x10D2BC: mov  w19, #1
0x10D2C0: adrp x8, 0x7002B000
0x10D2C4: strb w19, [x8, #0x472]     -> records success at 0x7002B472 (a WRITE, not a read)
0x10D2C8: bl   0x105384
0x10D2CC: mov  x0, x19               -> success return, x0=1
```
Two corrections to this investigation's own earlier reading, found and fixed during this round's `/검토`: (1)
`0x10D2C4` was first read as `ldrb w19, [x8, #0x472]` (a load) and the function's return value was described as
"coming from" that RAM byte; re-deriving the encoding bit-by-bit (`opc = bits23:22 = 00`, not `01`) shows it is
actually `strb` (a store) of the hard-coded `w19=1` set two instructions earlier -- the RAM byte records the
outcome, it does not produce it. (2) a review document's claim that `0x10D224`/`0x10D228` tested this same
`0x7002B472` byte (at `+2`) was checked the same way and found to be off by one instruction address (the real
`ldrb`/`cbz` pair sits at `0x10D224`/`0x10D228` with `imm12=0`, i.e. it re-reads `0x7002B470`, the function's own
argument byte written moments earlier at `0x10D194` -- an unrelated check). Both corrections were verified by
re-deriving the raw ROM bytes (recovered from this session's own pre-compaction transcript where the live
context no longer held them) and decoding every field (`opc`, `Rn`, `Rt`, `imm12`) independently.

Both paths above are consistent with the observed final return value 0, so which one the real device actually
takes -- an early failure at `0x10C51C`, or reaching `0x10D2B8` and failing there (`0x104E04` returning 0) -- is
not yet distinguished. The probe was retargeted again (same single-slot constraint as before) to these two
addresses to measure that directly:
  - `0x10D150`: `.x0` holds `w0`, masked to 32 bits (the `cbz` there only tests the low 32 bits)
  - `0x10D2B8`: `.x0` holds the full, unmasked 64-bit `x0` (the `cbz` there tests all 64 bits, unlike `0x10D150`)
Covered by the current `tests/roms/panic_probe.S` and `tools/c1vm-test.sh`'s "panic probe" check: a synthetic ROM
visits `0x10D150` with `x0=0x1_00002222` (upper bits deliberately set, to prove the hook's 32-bit mask is really
applied there), then `0x10D2B8` with `x0=0x5_00006666` (to prove the *second* address is captured unmasked), then
resets -- a passing run must report `hits=2`, `first.x0=0x2222` and `last.x0=0x500006666`.

Still open, gated on this round's measurement: whether `0x10C51C`'s failure (if that is what happened) traces to
a Henge device/MMIO gap, and separately, what `0x104E04` and the `0x7002B470`-based 3-byte table are actually
for -- `devices/cfg.c` was compared field-by-field against QEMU's independent `hw/vmapple` reimplementation of
the same AVPBooter boot process (same `VMAPPLE_CONFIG` base `0x00400000`, same field layout) and found to match
closely, so a misplaced or mis-shaped config struct is no longer a live hypothesis; QEMU's vmapple code also does
not pre-populate the `0x7002A000`-`0x7002B000` RAM region, which weakens (without disproving) the idea that a
*host* is supposed to fill it -- the `0x7002B470`-`0x7002B472` writes found this round show at least part of
that region is self-maintained by the ROM's own code instead.

## How this gets used

For a limited ROM boot trial -- the first one already happened once, on a real device, right after this
document was written -- each of the five stages above should be read off in order from that attempt's logs (`ui_runner_vm`'s own stdout plus a `"status"` poll or two, and
`VmDisplayActivity`'s status text) and reported as pass/fail/not-reached individually — not collapsed into a
single "did the Apple logo show up" verdict. A failure at stage 1 or 2 means stages 3-5 were never reached and
say nothing either way; stopping the report at the first failing stage, rather than guessing about the later
ones, is the entire point of separating them.
