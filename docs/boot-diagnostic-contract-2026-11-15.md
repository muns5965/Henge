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

## How this gets used

For a limited ROM boot trial -- the first one already happened once, on a real device, right after this
document was written -- each of the five stages above should be read off in order from that attempt's logs (`ui_runner_vm`'s own stdout plus a `"status"` poll or two, and
`VmDisplayActivity`'s status text) and reported as pass/fail/not-reached individually — not collapsed into a
single "did the Apple logo show up" verdict. A failure at stage 1 or 2 means stages 3-5 were never reached and
say nothing either way; stopping the report at the first failing stage, rather than guessing about the later
ones, is the entire point of separating them.
