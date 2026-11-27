# integ7 Henge device-test runbook (branch research/g7-integ7-henge)

Status: nothing here has been run on a device. Library pin to be updated after approval; this branch does not change it. The frozen comparison baseline is research/android-port 7cbc24b (integ5 + UI fix); do not compare a run of this build against it without saying so.

## Rules for every run
- One change per run. Everything not named in a row stays at the dialog default: JIT 64 MiB, exact stats OFF, NVRAM pre-seed OFF, display events OFF, VBL experiment 0, fb0 (F1) OFF, GPU-exec / dump checkboxes as in the baseline run you compare against (write them down).
- The dialog remembers nothing except what the log header says; copy the header lines (`g7-jit-config:`, `g7-gpu-options:`, `g7-engine-diag:`) into the notes of every run. If a header line is missing the build is not this one.
- A booted clone changes on disk. Start each run from a fresh clone of the same original (or record the clone name and do not compare a run that started from a dirtier clone). The S1 pre-seed writes into the clone's aux image: give it its own clone.
- Same run time (the "실행 시간" spinner) and same USB mode for runs that are compared. Phone cool, charger state noted, no other app in front.
- Collect after each run: the run log (g7_cboot_*.txt), run-info.txt, the export manifest, the `g7-jit:` / `g7-mem:` / `g7-diag:` lines, the stderr `stats` lines if the export has them, and screenshots with the time they were taken.

## Comparison points (beyond "reached the desktop", which has not happened)
Record the time since start (log `t=` or `g7-stage:` seconds) at each of these; "never" is a result:
1. First kernel: `g7-stage: kernel first seen at <s>` (a heuristic from pc samples, EL1 at 0xfffffe00...; says so).
2. First PVG frame: the runner's `frames published N (pvg ...)` line first showing pvg > 0, and the first `g7-vbl:` swaps count > 0 (swaps=).
3. Apple logo: no log marker exists; take it from timed screenshots / dumped frames (fb0 dumps if a dump folder is set) and from the `g7-fb0:` change=nz_up line times. Mark as visual.
4. Startup Disk screen (reached in run-20261127-152759-034): again visual; the swap count and the wbverify lines around it help date it.
5. Desktop (not reached so far).
Also per run: last swap time and what the final frame shows (black / cursor only), and whether the 10 s `g7-jit:` window shows `full=+0` for long stretches.

## Ordered checklist
R0. Baseline of this build (all defaults). Purpose: that the build boots as far as integ5 did, and the new fields exist. Look for: `g7-jit:` lines with `req=`, `dup=`, `ef_det=`, `first_s=`; `g7-fb0: start` ... `end` with `scans=`, `total_ms=`; `g7-nvram:` lines and the `summary`; the comparison points above. Stop and report before anything else if `ef_exh` is above 0, `ef_det` is above 0, or `req_emitfail` / `req_stw` are above 0.
J1. Only change: JIT 크기 = 256 (or marker `echo 256 > files/vm/jit_mb`, then press start yourself). Compare with R0: `full=`/`dropped_blocks=` growth per window, `req_region`, `dup_mb`/`dup_s`, `first_s` vs `retrans_s`, `majflt=`/`swap=` (the 256 MiB table may swap), `rss=`, and the time to the same boot stage. If J1 holds the boot longer or reaches a later stage, note it before running J2/J3.
J2/J3 (optional). 128, then 512, only if J0/J1 differ. J4: the better of 64/256 with "JIT 정확 통계" ON (extra up to 96 MiB; marker `echo 1 > files/vm/jit_exact`); read the `exact_*` fields of the stats lines, not the 10 s line. Never J4 for timing comparisons.
F0 (dedicated F1 run, integ7b: fb0 is default OFF, so R0 and J0/J1 do NOT sample). Only change: tick "초기 프레임버퍼 관찰" (not remembered; G7_FB0=1). Collect `g7-fb0:` lines, `nz_first_t=`, `dump=` lines (dumps need the dump checkbox; without it the log says NOT saved). Keep fb0 OFF in J0/J1 and every timing comparison. The F1 fb0 dumps and the PVG staging dumps are the evidence for Apple-logo quality and text legibility; a screenshot taken before the T9 full-resolution preview fix (96x64 hex preview) cannot be used to judge either.
S1. NVRAM pre-seed. Only after R0's `g7-nvram: summary` shows a recognised store. Only change: checkbox "NVRAM 사전 설정 auto-boot=true" (marker file `nvram_preseed`), fresh clone. Look for: the `g7-nvram:` line saying what was written (or why nothing was), what the guest reads/writes afterwards, and whether boot goes to a different stage than R0. Do not set boot-volume (format unknown). Collect the clone's macos_aux.img hash before and after if the export offers it (G7AuxCompare).
V1. Display events (VBL) checkbox ON, VBL experiment 0. Look at `g7-vbl:` init line (`display_events=on`), `delivered_bit*`, `swaps=`, the swap count at the end, last-frame state. Compare with R0 and with run-20261127-152759-034 (~151-153 swaps).
V2. Display events ON + VBL 실험 = tick_none. V3. Display events ON + bit3_once. V3b. bit3_on_enable (integ7b, display events ON): sends the one-time bit3 on the first tick AFTER the guest writes enable bit 3 (+0x104), not inside the MMIO write; counters show `bit3_sent=`, `bit3_sent_at_swaps=`. V4. swap_no_or2 (works with display events OFF or ON; keep it the same as the run you compare with and say which). One value per run; the `g7-vbl: counters exp=...` lines are the evidence (`tick_suppressed`, `swap_or2_skipped`, `delivered_bit3`).
After all runs: a table of run / the one setting / first kernel / first PVG frame / last swap / final frame / full flushes / dup_mb / ef_* / rss / swap / majflt.

## integ7b additions (research/g7-integ7b; device-verified = NO for all)
- T1: an emit failure on any vCPU now halts the whole VM (HV_HALT_ENGINE); the runner prints ENGINE-FATAL, the verdict kind is ENGINE_FATAL, exit status 4 (not a guest halt, not exit 0). MAX_WORDS/relocation overflow is recorded as a separate compatibility item (later block splitting), not as a dispatcher bug.
- T2: exact stats peak budget spinner (100/150/200/0 MiB, default 100, logged as `run_jit_exact_peak_mib=`); exact stats stays OFF by default.
- T3: g7-kill lines now say requested/confirmed/unconfirmed to match what actually happened.
- T4: NVRAM pre-seed backs up first (backup file beside the clone folder, not inside it), refuses on marker/free-space/write/flush failure and restores the region; clone-only. describeAll shows a cosmetic entry only.
- T5/T6: see V3b and F0 above.
- T7: library env flags the launcher can pass: LAGFX_ARM64_EXEC, LAGFX_ARM64_MAPPED_WRITEBACK, LAGFX_ARM64_DUMP(_DIR/_DATA), LAGFX_PIPELINE_MATRIX, LAGFX_VBL_EXP, LAGFX_LOG_*, and (new, integ7b) LAGFX_GPU_READBACK=1 and LAGFX_WB_VERIFY=1 as two separate checkboxes (default OFF, not remembered, never set to "0", removed from the inherited environment, logged as `gpu_readback=` / `wb_verify=` in `g7-gpu-options`). WB_VERIFY alone does not enable the independent readback; READBACK alone does. Readback adds a fence wait and may hide frame-freshness races: one flag per run. Every other library flag cannot be passed (no generic env mechanism by design). The runner scrubs only HV_*.
- T9 (sub-fix of the existing "display output" task, not a new task): the runner publishes the full-resolution frame atomically to `g7_frame.bin` next to the shm file (header G7FRAME1, BGRA, trailer G7FREND1 with the same seq; the reader rejects torn files). The FRAME hex line stays as a low-res diagnostic (96x64 limit). The file holds the GUEST framebuffer size as the runner read it (frame_shm), which may differ from the PVG surface size. Touch mapping uses the letterboxed guest size.

## Reading results (integ7b rules)
- F1 (fb0) runs only in the dedicated F0 run; J0/J1 and other timing runs have it OFF.
- Screenshots taken before the T9 full-resolution preview fix cannot judge Apple-logo quality or text legibility.
- When comparing Draw-skip counts, say whether the run used exact mode (MSAA refusals are counted as refusals) or approximate mode; the counts are not comparable across modes.
- A stub-substituted shader must not be counted as rendering success.
- Track four separate states per item: offline-pass (host/JVM tests), Android-compile (CI build), device-run, pixel-correct. integ7b: offline-pass only; the other three are NO.

## What this build cannot do
- No engine knob can be changed at run time from the phone (hv_knob_set is only reachable from host/main.c's "knob" command), so every setting above is a start-time choice.
- Blit op payloads 0x140/0x141 are undecoded and the 0x12f copy_texture_to_texture sibling bug is open (docs/research/followup-ledger.md, section 7): a GPU-side difference between runs may come from those, not from the setting under test.
- Counters are cumulative since start; the `(+n)` windows exist only for flushes, full, dropped_blocks, majflt and req. For per-window values of the others, subtract consecutive lines.
