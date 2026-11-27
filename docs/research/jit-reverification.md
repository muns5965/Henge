# JIT cache churn: independent re-verification (g7-jit2, corrected in g7-jit3)

Tags: [V] verified here (code read, log arithmetic, host test run); [I] inferred, reasoning shown; [N] needs the phone.
Branch `research/g7-jit2` from `research/g7-jit1-henge` (acb9142). The earlier conclusions (`/tmp/jit-report.md`, `/tmp/perf-report.md`) were re-derived from `core/dispatch.c`, `core/translate.c`, `host/jit_region.c` and the raw cboot log `220946a1-g7_cboot_1795760879032.txt` (31 stats lines, 2 vCPUs, `jit_mb=64`, 2048 MB guest).
g7-jit3 (branch `research/g7-jit3` from `research/g7-jit2` 29a7ff3) corrects items 3, 6 and 8 below, adds items 11 (the emit-failure loop and its fix) and 12 (exact-set memory), and changes one behaviour: a block that can never be emitted no longer flushes forever (item 11). Region-full behaviour, the TLBI policy, every default and the 64 MiB JIT default are unchanged.
Not changed: TLBI policy, any default, the JIT size (64 MB). Counters, the stats line, host tests and this document only.

Implemented / build-verified / device-verified, kept apart:

| | |
|---|---|
| Implemented (g7-jit3) | `core/emitfail.h` (new: the retry policy), `hv_translate_emit` returns a cause, `lookup()` uses the policy, three counters and a stats line, test seam `HV_TEST_HOOKS`, exact-set peak accounting and optional budget; `tests/test_emitfail.c`, `tests/test_jitstats.c` (`t_peak`), `tools/jitstats-test.sh` |
| Implemented (g7-jit2) | `core/jitstats.h` (new), counters in `core/machine.h`, `core/dispatch.c`, `core/machine.c`, `core/sysreg.c`, `core/translate.[ch]`; `tests/test_jitstats.c`; `tools/jitstats-test.sh`; `tools/check-jit-drop-sites.sh` |
| Build-verified (g7-jit3) | `tools/jitstats-test.sh` (plain and ASan+UBSan, x86_64): jitstats 100 081 checks and emitfail 30 checks, 0 failed; `tools/check-jit-drop-sites.sh` all ok (aarch64 syntax check of `core/*.c` included). `dispatch.c` and `translate.c` (also with `-DHV_TEST_HOOKS`) were compiled for syntax only; the emit-failure test runs a model of `lookup()`/`run_loop()` around the real `core/emitfail.h`, not the dispatcher itself (item 11). |
| Build-verified (g7-jit2) | host test (plain and ASan+UBSan, x86_64): 100 063 checks, 0 failed; static script: all ok, including `aarch64-linux-gnu-gcc -Wall -Wextra -fsyntax-only` of `core/{dispatch,machine,mmu,sysreg,translate,memop}.c`. The JIT itself was never run (no arm64 host, no qemu-user). |
| Device-verified | NO (g7-jit2 and g7-jit3). Nothing of the new counters has run on the phone. Every number below that is a counter value is still [N]; numbers marked [V] come from the old log and from source. |

## 0. What changed against the earlier report

| Earlier statement | Now |
|---|---|
| "TLBI never drops translations" | Confirmed from the call paths, and now guarded by a static check (item 7). |
| "85-98% of translations are retranslations" | Right in kind, slightly high: the bitmap over-counts. Corrected 84-97.8% in the t=20-52 s windows (item 2). The correction is about the size of the log's own rounding (whole percent). |
| "21-25% of CPU time is translating" | Mislabelled. It is wall-clock ticks spent inside `hv_translate_emit`, summed over both vCPU threads, divided by wall time: about 10.5-12.5% of each vCPU if both were equally busy, not 21-25% of CPU time. It also contains duplicate and discarded emits, and leaves out placement, publishing and lock waits (item 5). |
| "about 303 bytes per block" | It is `jit.used` / `nblocks` at the sample, i.e. reserved region (1 MB chunks, including duplicates and open chunks) per published block, not a block size. It matches the per-translation figure from `translated_words` (312-319 B), which is itself chunk-granular and can double count (item 3). Use it as an upper bound of the direct size. |
| "1.9 M unique blocks, 570 MB" | Arithmetic reproduces (1.90 M by t=120 s; 2.01 M after the collision correction, 575-609 MB at 303 B), but it is the total ever translated in the boot, not the simultaneous working set. The working set is not in the log (item 6). |
| "each block is executed about 1.2 times per cycle" | Not supported. `blocks` counts dispatcher entries (`stats.blocks++` in the dispatch loop, plain non-atomic), not block executions; chained jumps and indirect-branch-cache hits bypass it. Dropped. |
| "emit-fail could loop, open" | Read from code: it can (item 4), and it is an execution-stability defect, not a statistic: fixed in g7-jit3 (item 11). |
| "Duplicates cannot change the 84-98%, so removing them does not matter" (g7-jit2 item 8) | True only for the counting identity (duplicates are not in `translations`). It missed the indirect effect: a duplicate occupies code space, so it brings the region-full flush earlier (item 8). |
| "about 400 MB RSS for 256 MB" | It is the total JIT-related estimate (code plus about 150-185 B of metadata per block), not the increase; the increase over 64 MiB is 287-309 MiB on those assumptions (item 3, table). |
| `exact_gap` as "reuse distance" (g7-jit2 item 2) | It is a flush-epoch gap between two translations of a block, not an execution reuse distance and not a cache-size optimum (item 6). |
| Not considered earlier | Duplicate translations by two vCPUs, per-cause flush requests against executed flushes, first versus retranslation time, per-block direct size, memory cost of metadata per cached block. |

## 1. Duplicate translations (lookup, two vCPUs) [V code, N count]

`lookup()` (core/dispatch.c): `find()` without the lock; on a miss the block is emitted into the thread's buffer (`hv_translate_emit`), placed into the region without the lock (`hv_translate_place` -> `chunk_take` -> `hv_jit_reserve`, one relaxed `fetch_add`), and only then `tr_mu` is taken and `find()` repeated. If another CPU published the same (VA, PA, flags, gen) in between, the new block is dropped and `b` of the winner returned; the loser's code stays in the region until the next flush (the source comment says so). So the cost of a duplicate is its emit time plus its region space. It is not in `translations`, not in `retrans_*`, not in `translated_words`; it is in `translate_ticks` and `translated_insns` (counted right after emit).

Window: from the first `find` miss to the publish: emit + place. After every flush both vCPUs start cold and run the same kernel code (scheduler, idle loop, interrupt paths), so duplicates should cluster just after flushes [I]. With 2 vCPUs and about 5 us between one CPU's translations the overlap probability is not small; whether it is the same key is the unknown. No bound follows from the log [N].

New counters (`--stats`): `duplicate_translation_count`, `duplicate_translation_words` (direct words of the dropped copy), `duplicate_translation_ticks` (its emit time); `epoch_dup_words` per flush. Printed in the `code:` line (count, MB, seconds of emit) and as a share of the interval in the `translate wall time` line.

## 2. The hashed "seen" bitmaps (85-98%) [V]

What they are: `seen_va`, `seen_vapa`, each `1 << 18` uint64 = 2 MiB = 2^24 bits; index = top 24 bits of `((va>>2)^flags) * 0x9e3779b97f4a7c15` (and, for VA+PA, xor `pa * 0xc2b2ae3d27d4eb4f` before the shift). Static, so shared by every machine in the process; never cleared on a flush; no page generation. Written only under `tr_mu` at publish. They answer "has a block with this VA+flags (VA+PA+flags) ever been published in this process": a block translated again after a flush, or after its page changed, both count as "again". No false negatives; false positives when a never-seen key hits a set bit.

Error, analytic: with D distinct keys inserted, a new key finds its bit set with q = 1 - exp(-D / 2^24). Observed again = true again + new * q, so the report is always >= truth.

Synthetic check (tests/test_jitstats.c, run here, plain and ASan/UBSan): the first-time false-positive rate matches q averaged over the insertion within 0.1 pp for random 48-bit VAs/PAs; for the VA+PA bitmap also for ordered kernel-text-like and per-page user-like keys (1.46% vs 1.475% at 0.5 M, 5.72% vs 5.73% at 2 M, 11.00% vs 11.03% at 4 M). For the VA-only bitmap structured keys collide less than random (an arithmetic run through a multiplicative hash), never more. The exact value for the real guest's address pattern is [N] (needs the exact set, below), but the analytic figure is the right one for the VA+PA bitmap.

What this does to the log (iterating q with the running distinct count; the log prints whole percent, so +-0.5 pp noise):

| t | reported again (va+pa) | corrected | distinct so far | bitmap full |
|---|---|---|---|---|
| 10 s | 60% | 59.0% | 0.83 M | 4.8% |
| 20 s | 85% | 84.0% | 1.35 M | 7.7% |
| 31 s | 91% | 90.1% | 1.75 M | 9.9% |
| 41 s | 98% | 97.8% | 1.85 M | 10.4% |
| 52 s | 97% | 96.6% | 1.95 M | 11.0% |
| 646 s | 93% | 91.9% | 2.52 M | 13.9% |

How to qualify the figure: "85-98% of translations in t=20-52 s are blocks whose VA+PA+flags was published before in this boot (bitmap, 2^24 bits; over-counts new blocks by 0.2-1.2 pp here; no page generation, so retranslation after a code change is included too)". Code-change retranslation is small in this log: `smc` is 3/s in the first window and 0 afterwards, `ic` 0-1/s [V], so almost all "again" is flush-driven [I; the exact set splits it: `exact_again_same_gen` vs `exact_again_new_gen`].

Exact option (opt-in, implemented): `HV_STATS_EXACT=<max entries in thousands>` (default 2048; also read as `G7_STATS_EXACT`, because the Android clone runner scrubs every `HV_*` variable, host/ui_runner_vm_clone.c:212; the launcher does not set it, so the phone cannot use it until the launcher passes it, not done here). One open-addressing table of (VA, PA|flags) -> (page gen, flush epoch) under `tr_mu`; 24 B per slot, doubling from 4096 slots, at most 2^22 slots = 96 MiB, no insert beyond 70% load (counted in `exact_overflow`). It reports: new, again with the page unchanged (dropped by a flush), again after a code change, bitmap false positives (`exact_bitmap_false`), and the retranslation gap in flush epochs (0, 1, 2, 3-4, 5+). The gap histogram is the first real evidence on reuse distance. Tested against an independent reference (400 k operations), overflow behaviour, and a cyclic working-set simulation (every block comes back exactly W/C = 3 epochs later, bitmap over-count 2 381 of 1.5 M).

## 3. translated_words, locking, and the 303 B/block [V code, I effect]

Placement takes no lock: `chunk_take` reserves 1 MB (`JIT_CHUNK_WORDS` = 262 144 words) per CPU and generation with a relaxed `fetch_add` on `jit.used`, and a block is carved from the CPU's own chunk (hot part from the front, cold part and pool from the back). `translated_words += m->jit.used - used0`, with `used0` read before `hv_translate_place` and `jit.used` read after the `tr_mu` acquisition. Therefore:
- the delta is 0 for most blocks and 262 144 words for the one that opened a chunk (granular, so the per-block figure is meaningless, only sums are);
- a chunk another vCPU opens inside this window (place + lock wait) is added here as well: other vCPUs' concurrent placement can inflate the total, by up to the fraction of chunk grabs that fall in the other CPU's window [I]; no lock excludes it;
- it includes the chunk tails lost at a flush and the duplicate/unpublished copies reserved in the chunk (those are not published, but the next published block's window may include their chunk grab).

Direct size per block: new `published_words` = `nhot + pad + ncold` of each published block (`hv_translate_last_place_words()`, read right after placement on the same thread), compared in the `code:` line with the `translated_words` figure (`x1.xx`). Per-flush breakdown: reserved (`flushed_words`) = live (published) + duplicate + unpublished + other (chunk slack, open chunks, padding).

Where 303 B came from [V]: `jit.used*4 / nblocks` of the sample at t=81 s: 64 MB / 221 389 blocks = 303 B. Cross-check from `translated_words`: 16.1 words/insn x 4.9 insns x 4 B = 315 B at t=31 s (312-319 B across the five boot windows). They agree within 4%, which is not an independent confirmation: both measure reserved region per published block, so both include duplicates, unpublished copies and chunk slack. The direct size is at most 303 B, so `translated_words` was inflated by at least about 4% in that window (double counting and slack together) [I]. The upstream comment at core/machine.c:~84 gives 296-300 B/block on the Mac (HV_RECHECK=1) and 259-261 B without, so the order of magnitude holds [V quote].
Slack bound: at most one open 1 MB chunk per CPU lost per flush = 2 MB of 64 MB (3%) plus the region being declared full when a CPU cannot get a whole chunk [V code].
Memory side cost not in earlier report: per cached block `HvBlock` is 80 B, plus up to two exit slots at 52 B each (slots, site, stub, cond, xa, xpa, xchained), so about 150-185 B of metadata per block on top of ~300 B of code (`max_blocks = bytes/160`, `max_slots = 2*max_blocks`, calloc, touched lazily) [V sizes from machine.h]. A 256 MB region full of 303 B blocks is ~880 k blocks and roughly 127-156 MB of metadata: about 383-412 MB in total, the figure quoted before as "about 400 MB". That is the total JIT-related estimate at 256 MiB (code + metadata), not the increase [I]. Per region size, with 303 B of code and 150-185 B of metadata per block, every block placed (blocks run to region-full; not touched pages that were never used):

| `--jit-mb` | blocks (k) | metadata (MiB) | total JIT-related (MiB) | increase over 64 MiB (MiB) |
|---|---|---|---|---|
| 64 | 221 | 32-39 | 96-103 | 0 |
| 128 | 443 | 63-78 | 191-206 | 96-103 |
| 256 | 886 | 127-156 | 383-412 | 287-309 |
| 384 | 1 329 | 190-234 | 574-618 | 478-515 |
| 512 | 1 772 | 253-313 | 765-825 | 670-722 |
| 768 | 2 658 | 380-469 | 1 148-1 237 | 1 053-1 134 |

Computed here from 303 B/block (an upper bound of the direct size, see above), so the block counts and the metadata are slightly high [I]; the metadata sizes are from `machine.h` [V], the resident part depends on how much of the region and tables is touched (calloc, lazy) [N]. The extra RSS the phone would see at 256 MiB is therefore about 290-310 MiB, not 256 and not 400.

## 4. Flush causes, requests against executions [V code, N counts]

Every request, by site:

| Site | Counter (old kept) | New counter |
|---|---|---|
| `hv_translate_place` fails (region full) | `flush_full` (old) | `flush_code_region_full` |
| `nblocks >= max_blocks` at publish | `flush_full`, `flush_table_full` (old) | (`flush_table_full` is the cause counter; request total below) |
| `hv_translate_emit` fails | `flush_emit_fail` (old) | request total |
| flush already pending when the block is looked up | none before | `flush_pending_at_lookup` |
| flush already pending at publish (table not full) | `flush_full` (old, so it also counts this) | `flush_pending_at_publish` |
| `hv_knob_set`, `HV_IC_FLUSH` | none | `flush_req_other` (host/main.c tools set `flush_pending` directly and are not counted) |
| any of the above while a stop-the-world flush is under way | none | `flush_req_during_stw` |
| all | | `flush_requests` |
| executed | `block_flushes` (old) | printed as "executed" |

Consequences: `flush_full` (the log's "full flushes") counts requests that went through `lookup`'s `flush:` label, not flushes: it is region-full + table-full + pending-at-publish, and can exceed `block_flushes`. In the log the per-second rates of the two agree in every window, but at 1-2 per second the integer print cannot show a 10% difference [N]. `stop_the_world` clears `flush_pending` before the other CPUs park, so a CPU still running can set it again and cause an immediate second flush that throws away the blocks of the first few microseconds: `flush_req_during_stw` counts that.
Table-full is excluded by arithmetic for 64 MB: `max_blocks` = 419 430; the region fills at 221 389 [V]. For 256 MB: 1 677 721 against ~880 k, also region first. Region-full as the cause of the 67 flushes in t=10-52 s: 14.6 M translations / 67 = 219 k blocks per cycle, equal to the 221 k at the fill sample [V]; the new counter will say it directly [N].
Open finding (g7-jit2), fixed in g7-jit3 (item 11): an unemittable block made `lookup` request a flush and return NULL; the caller saw `flush_pending` set and retried (`continue`), the flush ran, the same block failed to emit again, request again. Nothing bounded it, so a block that can never be emitted gave a flush storm instead of "cannot translate block" [V code read, occurrence unknown]. `flush_emit_fail` > 0 on the phone would show it.

## 5. translate_ticks [V code]

- Unit: `mach_absolute_time()` = `CLOCK_MONOTONIC` at 24 MHz (host/platform.h): wall clock, not thread CPU time. Descheduling by the host inside the emit counts.
- What is timed: only `hv_translate_emit` (decode and generate into the thread buffer). Not placement, not the publish lock wait (`tr_wait_ticks` has that), not the `find` before it. Two `clock_gettime` calls per translation are inside the cost (about 0.67 us per translation in this log, so the clock reads may be a few to 15% of the figure [I]).
- Who adds: every vCPU thread, previously with a plain `+=` (lost updates possible); now a relaxed atomic add (same field, same meaning).
- Included: duplicates, blocks dropped after placement and blocks dropped because placement failed (the emit already happened).
- Original formula (core/dispatch.c print_stats): `100.0 * (s->translate_ticks - last.translate_ticks) / (now - last_t)`, i.e. summed emit wall ticks over both vCPUs, divided by the wall time of the interval since the previous print. With `cpus=2` that sum can reach 200%. The reported 21%, 25%, 24% (t=20, 31, 41 s) are therefore about 10.5-12.5% per vCPU if both emit equally [I]. The earlier "21-25% of CPU time" is not what the number measures.
- First versus retranslation: the old figure did not distinguish. New: `first_translation_ticks` / `retranslation_ticks` (bitmap, so with the item 2 error), `exact_first_ticks` / `exact_retrans_ticks` (exact), `duplicate_translation_ticks`, `discarded_place_ticks`, `unpublished_ticks`; the `translate wall time` line prints each as a share of the interval, and the cumulative seconds. They are classified at publish under `tr_mu` from the emit time measured earlier. With 84-98% retranslation, roughly 85-98% of the emit time is retranslation if first translations cost the same per block [I]; first translations of a boot may be more expensive (cold decode); the counters answer it [N].

## 6. Unique blocks, 570 MB, working set versus total [V arithmetic, I meaning]

Re-derived from the five raw windows (rate x window length, window lengths from the stats timestamps, +-0.5 s): translations per window 2.03 M, 3.20 M, 4.11 M, 4.20 M, 3.13 M; new (1 - again) 0.81 M, 0.48 M, 0.37 M, 0.08 M, 0.09 M; cumulative 1.84 M at t=52 s, 1.90 M by t=120 s (earlier report: 1.9 M, reproduced); with the collision correction 1.95 M and 2.01 M. At 303 B: 575 MB observed, 609 MB corrected (an upper bound per block). Whole log (to t=856 s): 2.48 M observed, 2.69 M corrected, 750-815 MB.
This is total distinct code translated during the boot, a sum over time. It is not the working set: the log cannot give the code that is needed within any one time slice. What the log bounds: lower, at least one cycle's worth, 219 k blocks (66 MB) are touched within 0.63 s; upper, the 2.0-2.7 M distinct overall. Between these lies 64 MB to 800 MB. The new `exact_gap` histogram and the effect of a larger region on the flush rate are the two ways to place it. What `exact_gap` is: for each retranslation, the number of flush epochs between that block's previous translation and this one (`epoch - old_epoch`, counted in whole flushes). It is a gap between translations, not an execution reuse distance: a block executed a million times in an epoch has gap 0 or 1 like one executed once, and a block that was not needed for five epochs and is then needed has gap 5+ however hot it was before. It is also not a cache-size optimum: a gap of 1 does not say that a region twice as large would have kept it (that depends on how much is translated in between, which the epoch length hides), and a large gap says only that the block was not needed for a while. Read it as "how soon after being thrown away did the code come back": mostly gap 1-2 means that most of the churn is the same code coming back each cycle (capacity or eviction can help), a heavy 5+ tail means long-lived code that is re-needed rarely (a bigger region helps less).
Live versus allocated: with a region too small the live code at flush time is always ~ the region (it is full by definition), so `live_words_at_flush` only says something once the region is large enough that flushes get rare: then live-at-flush below the size means the working set fits. New: per flush average reserved, live (and max live), duplicate, unpublished, other, in MB.

## 7. TLBI, from the call paths [V]

- Whole flush (`hv_flush_blocks`, `hv_jit_reset`): reached only through `flush_pending`, set by `lookup` (region/table/emit/pending), `hv_knob_set` (machine.c:42; called only from host/main.c:609 stdin "knob" and ios/Henge/Native/henge-vm.c:116, not from the Android clone runner), `HV_IC_FLUSH` (sysreg.c:471, env, off by default), and the Mac tool (host/main.c:420, 648). Executed by `stop_the_world` (dispatch.c) and by tests.
- Per-page invalidation: `page_gen[pg]++` happens only in `hv_invalidate_code` (machine.c:465), whose callers are CPU stores to code pages (memop.c:418, machine.c:577), DMA write-back (`hv_dma_written`, machine.c:534) and IC IVAU only with `HV_IC_IVAU` (sysreg.c:461). Block lookup keys include `page_gen`, so those, and only those, stale blocks.
- TLBI (`hv_tlbi_apply`, sysreg.c:343, and the `hv_tlb_flush*`, `hv_tlb_stash_drop`, `hv_ijc_drop_*`, `hv_ijc_reset` family it calls): software TLB, stashes, indirect-branch cache. None of these function bodies mentions `flush_pending`, `hv_flush_blocks`, `hv_jit_reset`, `page_gen`, `hv_invalidate_code`, the block hash, `nblocks`, `jit.used` or `jit_gen`. This is now a script, `tools/check-jit-drop-sites.sh` (it also fails if a ninth drop site appears; mutation-tested here by adding `flush_pending` to `hv_tlb_flush_va_page`). A TLBI can make a block miss only indirectly: an emptied TLB or IJC sends the CPU through `lookup`, which finds the existing block (`find`) unless its key changed. No policy change; the lazy policy asked for is what the code does.
- The log's "cache drops/s: ASID / user VA / kernel VA" are IJC drops, not translation drops (unchanged from the earlier report).

## 8. Verdict: capacity first, then duplicates and selective eviction (revised in g7-jit3)

The earlier text said that duplicate removal "cannot reduce" the 84-98% and cannot matter. That is true only for the counting identity: duplicates are never counted in `translations`, so the retranslation share (retranslated / published) does not contain them. It is not true for the flush rate. A duplicate keeps its code space until the next flush (`lookup()` publishes the other CPU's copy and leaves the dropped one in the region), so it consumes region and brings the region-full flush earlier. Removing duplicates therefore can reduce the number of flushes and, indirectly, the number of retranslations and the emit time spent on them.
Estimate, [I]: if duplicates (and unpublished copies, which behave the same) are a share d of the region's reserved words at flush, the live code per cycle is (1 - d) of the region, the flush interval grows by about 1 / (1 - d), and flushes and the retranslations they cause fall by about the same factor. Examples: d = 5% gives 1.05x, d = 10% gives 1.11x, d = 25% gives 1.33x. Only the measured d answers it: per flush `duplicate` and `unpublished` MB against `reserved` in the `code:` line (`dup_words_at_flush_sum`, `unpub_words_at_flush_sum`) [N]; the second-order effect (fewer flushes also means fewer duplicates just after each flush) goes the same way.
Evidence for ordering the levers:
- Capacity acts on the dominant cost: ~91-98% of the 374-420 k translations/s in t=31-41 s are blocks flushed and made again, about 0.6 s apart; the flush rate (1.6/s) is the region-fill rate [V]. It is the easiest to test (one number, `--jit-mb`), needs no code, and its cost is the table in item 3 (increase over 64 MiB: 96-103 MiB at 128, 287-309 at 256, 670-722 at 512, in the same table). It is a cliff, not a line: little changes until the region approaches the working set, which lies between 66 MB and 800 MB [I].
- Duplicate avoidance (for instance claiming the key before emitting, or making the second CPU wait for the first) is worth doing exactly in proportion to d above; it also cuts emit time by the duplicates' share. It needs a design that does not serialise the lock-free translation, so measure d first.
- Selective eviction (drop part of the cache on a flush instead of all of it) attacks the same flush cost without the memory; not decided by any data here, and the hardest to make safe (chaining, `page_gen`, slots).
Order: (1) capacity A/B, J0 (64) against J1 (256), the exact set off; (2) read d from the same logs; (3) duplicate avoidance if d is a few percent or more; (4) selective eviction if 256 MiB does not remove the churn. Evidence still missing: (a) device A/B with the counters; (b) `duplicate_translation_*` in a real boot; (c) `exact_gap` (the launcher passes `G7_STATS_EXACT` since g7-hjit3); (d) real RSS/swap at 256; (e) `flush_emit_fail`, `emit_fail_*`, `flush_req_during_stw` values.

## 9. Phone run (same boot, `--jit-mb` 64 then 256; the launcher constant is G7CloneBoot.java:326, an experiment build, default unchanged)

Per run, from the stats lines (every 10 s) and the end of the log:
1. `flush requests: ... code region full N ... executed M`: expect at 64 MB `code region full` ~ `executed` ~ 98 at 856 s; at 256 MB far fewer. If `emit fail` or `during stop-the-world` is non-zero, say so before anything else.
2. `translations dropped by whole flushes: ... avg X blocks, Y MB of code each`: Y near the region size means capacity; at 256 MB, Y well below 256 means the working set fits.
3. `code:` line: `published B/block direct` vs `translated_words says` (x ratio), `duplicates N (MB, s emit)`, per flush reserved / live / max live / duplicate / unpublished / other. Compare live-at-flush at 64 and 256 and the per-block bytes.
4. `translate wall time` line: first vs retranslated share and cumulative seconds; the share should fall in step with the flush rate; duplicates share.
5. Main line: `translated/s`, `(again: va % va+pa %; N% of time ...)`, `blocks held`, `full flushes` (cumulative), per window t=10-52 s; wall time to the desktop (the number the user cares about).
6. Memory: RSS of the runner, swap, `available`, at the end of the boot window, both runs.
7. Optional, after the launcher passes `G7_STATS_EXACT=2048`: the `exact seen-set` line (again same page vs changed, `epochs since last translation` histogram, `bitmap said again for a new key`); extra 24 B x slots of RSS (up to 96 MiB), so a separate run.
Reading: churn is capacity if the flush count and retranslation time drop sharply between 64 and 256 and `duplicate` stays small; if the flush count barely drops at 256, the working set is above 256 MB and eviction policy is the lever.

## 10. Could not be verified (see also item 11 and 12)

The JIT (lookup, placement, flush, the new counters in the stats line) ran nowhere: no arm64 host. Only syntax/warnings on aarch64 and the host tests of `jitstats.h`. The `print_stats` formatting with the new lines, the atomic counters under real two-vCPU load, and the per-flush accounting at `hv_flush_blocks` are not exercised. Duplicate rate, emit-fail occurrence, true working set, real FP rate of the guest's key pattern, and memory at 256 MB are all [N].

## 11. The emit-failure loop and its fix (g7-jit3) [V code and model test, N device]

An execution-stability defect, not a statistic. Read from `core/translate.c` and `core/dispatch.c`.
What can fail, and what `lookup()` did:

| Call | Result | Cause class | Before | Now |
|---|---|---|---|---|
| `hv_translate_place` | -1 | region full (`chunk_take` -> `hv_jit_reserve` found no room) | `flush_full++`, flush requested, retry | unchanged; also counted in `emit_fail_region` |
| `hv_translate_emit` | -1 | block over the emit buffer (`nhot`/`ncold` > `MAX_WORDS` = 16384) | flush requested, retry, same failure: endless | `HV_TR_E_TOO_BIG`: policy below |
| `hv_translate_emit` | -1 | relocation table overflow (`MAX_RELS`, in `plink`) | same | `HV_TR_E_OVERFLOW` |
| `hv_translate_emit` | -1 | grouped-access copies did not fit (`emit_group_copies`) | falls back to the plain translation inside `hv_translate_emit` (HV_GROUPS=1 only), so only the second try can fail | `HV_TR_E_GROUP` (internal) |
| unsupported guest instruction | not a failure | `translate_one` emits a helper call that raises the guest's UNDEF (`HV_OP_UNDEF`) | n/a | n/a |
| (nothing) | | transient emit failure: none exists today (the emit buffer is per thread, nothing is allocated) | would have worked by retry | a failure that goes away still does: it gets a flush and a retry |

All three emit failures depend on the block (guest words at this VA, flags, page generation) and on constants, not on the region or on timing; a flush does not change them. The block loop's own limit (`WORDS_PER_INSN_MAX * 2` headroom, `MAX_RELS` check) is meant to keep them from happening, so in practice they should not; `flush_emit_fail` > 0 on the phone would be the first evidence. No interpreter or slow path for a block exists in the code (the dispatcher only runs translated blocks), so none was invented.
The loop: `lookup` NULL with `flush_pending` set -> `run_loop` `continue` -> `stop_the_world` flushes -> `lookup` -> same emit failure -> flush again, forever, with every vCPU parked in each flush and no log line.
Fix (`core/emitfail.h`, used by `lookup`): the failing key (va, pa, flags, page gen) is remembered per CPU thread. The first `HV_EMIT_FAIL_MAX_FLUSHES` = 1 failure asks for a whole flush exactly as before (cost unchanged for a failure that is not the block's own). The next failure of the same key gives up: no flush is requested, `lookup` returns NULL with nothing pending, and `run_loop`'s existing path prints `hengevm: cannot translate block at <pc>`, parks the CPU and returns `HV_EXIT_UNSUPPORTED`; `lookup` first prints one line with the CPU, va, pa, flags, gen and the cause name. A different key, or a block that emits, resets the allowance. Another CPU's concurrent flush is handled by the same `continue` as before and cannot repeat without bound either (the count is kept, so the next failure gives up). `hv_translate_emit` now returns `HV_TR_E_*` instead of -1 (callers only test for nonzero); `hv_translate_block` still returns -1.
Counters (always kept, shown in the stats line `emit/place failures (g7-jit3)`): `emit_fail_region` (placements that found the region full: flush and retry as before), `emit_fail_deterministic` (emits that failed), `emit_fail_retry_exhausted` (blocks given up). `flush_emit_fail` keeps counting the flushes the emit failures caused (now at most one per key).
Test seam: `-DHV_TEST_HOOKS` adds `hv_test_emit_hook` / `hv_test_place_hook` (function pointers called at the start of `hv_translate_emit` / `hv_translate_place`; nonzero makes the call fail) in `core/translate.c`; the limit is `hv_emit_fail_max_flushes` (-1 is the old behaviour). They exist for a Mac or arm64 test of the real dispatcher and were only compiled for syntax here.
Test, `tests/test_emitfail.c` (`make test-emitfail`, `tools/jitstats-test.sh`; any CPU): a model of `lookup`/`run_loop` around the real `hv_emitfail_note`/`hv_emitfail_clear` with injected emit/place failures. Results: region-full only, 100 blocks into a 10-block region: 9 flushes, all blocks succeed, 9 `emit_fail_region`, 0 deterministic, identical to the old policy in flushes, translations and emit calls. A block that never emits: old policy (limit -1) reaches the bound of 100 000 loop iterations with 50 000 whole flushes (it would go on); new policy at limit 0, 1 (the default) and 3: outcome "cannot translate" after exactly 0, 1 and 3 flushes and 1, 2 and 4 emit attempts; one `emit_fail_retry_exhausted`. A transient failure (fails once) succeeds after one flush and the allowance is fresh for the next block or generation. Good blocks around a bad one keep working. Limit of the test: it is a model of the control flow, not the dispatcher; the part that runs for real is the decision function and the counters' logic as copied into the model; the call in `lookup()` and the stats line are build-verified (syntax for aarch64) only [N device].

## 12. Exact seen-set memory (g7-jit3) [V source and test]

`hv_jstat_exact_grow` allocates the new table (`calloc(nn)`), moves every entry, then frees the old one. So old and new tables are alive together during a doubling: the peak is old + new, 1.5 x the new size. With 24-byte slots (`HvJstatSlot`: va 8, pa|flags 8, gen 4, epoch 4) and doubling from 4096 slots: 2^21 slots = 48 MiB, 2^22 = 96 MiB; the last doubling holds 48 + 96 = 144 MiB for the moment of the copy, then 96 MiB stays. The true maximum is 96 MiB steady / 144 MiB peak, holding at most 2 936 012 keys (70% of 2^22); the default of 2048 thousand entries needs the 2^22 table (2 048 000 / 0.7 > 2^21). The earlier text said "at most 2^22 slots (96 MiB)" and did not say the peak. The pages are touched quickly (a hash scatters), so the resident size is the full table soon after a doubling. [V: `t_peak` in `tests/test_jitstats.c` measures it: table 96 MiB, peak 144 MiB, 2 936 012 keys, then overflow.]
Cheap reduction, implemented: the table and peak bytes are measured (`cur_bytes`, `peak_bytes`, shown in the `exact seen-set` stats line) and an optional budget (`HV_STATS_EXACT_PEAK_MIB` or `G7_STATS_EXACT_PEAK_MIB`, default none) refuses a growth whose old + new would pass it, before allocating, counting the keys that no longer fit in `exact_overflow` like a full table (keys already held are still found). Example: 100 MiB stops at the 2^21 table: 48 MiB, peak 72 MiB, 1 468 006 keys; tested. With no budget, nothing changes (results are identical, default stays OFF). Not done: in-place or incremental rehash; an in-place doubling of an open-addressing table with wrap-around clusters is not a few lines and was not worth the risk for a diagnostic. When the exact set is used on the phone: either leave the 144 MiB peak in mind (a run on its own, J4), or set the budget.
