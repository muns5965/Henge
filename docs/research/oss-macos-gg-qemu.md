# merckhung/macos_gg_qemu comparison (2026-11-27) - docs only, GPL code NOT read

Method: README, LICENSE, patches/UPSTREAM-NOTES.md, docs/verification.md (the last two via fetch-tool summaries, one rate-limited), MODULE.bazel pins. No patch/QEMU/Reims code was opened or used. Reports: /tmp/mgq-A.md, -B.md, -C.md. The developer's own test numbers (Reims suites, 28 apps, login ~5 min) are NOT independently verified.

## Licences (use behaviour statements only, never code)
| component | licence (as stated) | MIT library may use |
|---|---|---|
| macos_gg_qemu own build/launch code | GPL-2.0-or-later | behaviour only |
| QEMU-Reims 4895365 | GPL-2.0 (per-file not checked) | behaviour only |
| Reims vGPU 12adb1fb | GPL-2.0-or-later OR LGPL-3.0-or-later | behaviour only |
| metal2vulkan f6a96b7 | LICENSE: GPL-2.0-or-later OR LGPL-3.0-or-later (licenses/README says LGPL-3.0-or-later only: disagreement, repo LICENSE trusted) | behaviour only |

## Findings relevant to Henge (all proposals; nothing implemented)
- R32Float: README only (older reims 69a57dd refused R32Float colour attachments: blank Dock icons/Terminal text; fixed on 12adb1fb, cause/fix not documented). Ours: id 55 -> R32_SFLOAT mapped and unit-pinned (not in any device dump); image creation checks transfer features; missing COLOR_ATTACHMENT feature drops the usage bit silently -> later target_begin fails, `no_target` counter only; blend feature never queried. Adreno/lavapipe attachment support unverified. Proposed row P07 (class B).
- Cubic sampler: reference falls back cubic->linear (once-per-process warning, strict switch refuses). Ours: runtime samplers cannot express cubic; constexpr filter 2 (bicubic) is silently mapped to linear, address values >4 wrap (% 5), no counter/log. Proposal: shared helper counting+logging each approximation kind once, optional strict refuse. Census X06 text looks stale (constexpr fragment wiring exists) - re-check by test before editing.
- MSAA: reference promotes an unsupported guest sample count to a supported host count while keeping the guest value (copies translated). Ours: texture MSAA creation fails with TEXWHY_C_MSAA; every host sample site is 1_BIT; pipeline sample count not decoded (O01); no resolve; device info advertises MSAASamples=4 (load-bearing). Not on the critical path; cheap next step = decode pipeline sample count + counter.
- Completion order (ours, src/): dispatch -> exec_end -> submit with readback copy -> fence wait -> exec_finish/ms_finish_down (guest write) -> stamp (submit_end, complete_stamp_slot_noirq, IRQ) -> display_swap. Immediate mode: write before stamp, except write failure (not retried) or bounded-wait timeout (stamp deferred). Swap mode: stamp may complete while the surface is still owed ("a late frame shows one swap later"). Default (env unset): flag-2 surfaces are never written back. INFER: stale pixels possible in swap/default mode; LAGFX_WB_VERIFY=1 separates "write missed" from "written but guest not looking".
- Speed: their login ~5 min with 8 vCPU/8 GiB TCG; post-login black desktop (460 MB 4K HEVC wallpaper vs ~30 s agent limit), Safari GPU init 4.4 s vs 3 s watchdog. Henge: 2 vCPU/2 GiB own JIT; a 3-minute window is probably too short for a login-level check (INFER). These are post-login issues; no evidence they relate to the missing Apple logo. Existing indicators for a longer run: g7-alive/g7-stall (progress=yes|no), cpu thread %, vmrun pc/EL.

## Census row proposals
P02 keep B (+note: mapping done, risk at attachment stage); new P07 B (no attachment/blend feature check, silent no_target); P01 note 55 mapped but not in dumps; P03 "gaps (30,55,65,71,81)" looks stale (diff tables first); S02 keep A + ordering note; F01/M13/X03/X08/X09/Adreno -13: no direct fix found in these docs; O01 cross-reference P07; census header HEAD (e9ad699) is stale vs library ece74ae.
