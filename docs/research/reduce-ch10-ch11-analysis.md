# CH10 / CH11 offline analysis (Adreno 840, -13 ladder, bundle pipefail_b7d3e8365670fd6c)

Legend: [V] verified (command run / file:line read), [I] inferred, [N] not checked.
Work dir /tmp/ana-ch (rows/, rows2/, f/*.spv, dis/*.dis, py/*.py, stats.txt, taint.txt). Library /tmp/wt-integ2 @ 8aa26da. No repo file was changed.
Offline tools: spirv-dis/spirv-val/spirv-opt installed (/usr/bin). The analysis scripts are python3 -I over `spirv-dis --raw-id` text.

## 1. Offline row generator and regeneration check
- [V] Emitter exists: `examples/reduce-rows.c` (built as /tmp/wt-integ2/bld/examples/reduce-rows, meson target examples/meson.build:142-145). It runs `lagfx_pipeline_reduce_run` in DRY mode (cfg.dry=1, dump_dir): pipeline_reduce.c:267-278 writes `<fh>.<row>.fs.spv` + manifest.tsv, and treats every row as FAIL (so adaptive rows follow the worst case). Usage: `reduce-rows <fs.spv> <outdir> [<dir with *.red.*.fs.spv>]`. Rewrites live in spv_reduce.c (lagfx_red_apply); no device needed.
- [V] Binary (10:19) is newer than the sources (pipeline_reduce.c 09:54, spv_reduce.c 08:21). Device log says lagfx=ece74aec; ece74aec is an ancestor of 8aa26da and `git log ece74aec..8aa26da` touches none of spv_reduce.[ch]/pipeline_reduce.[ch] [V].
- [V] Ran it on bundle fs.spv (sha16 ed21950f7f98a5d0 = log orig). Comparison of (bytes, inst, sha) with the device log:
  - run 1 (no file dir): 92 common rows, 0 mismatches. run 2 (with the red assets dir /home/claude/henge/android/app/src/g7exec/assets/red): 103 common rows, 0 mismatches.
  - Requested rows, all identical: CH10:3ff 97196 B/6435/6fa630e4bc6d5713; CH11:7ff 97196/6435/c6c248ee701950d0; RI8:6ff 219844/13398/fc645db1b069a2df; RI6:7bf 97372/6445/8adf613636fee8cd; IFC 51572/3043/5d040ed57a5aea02. Also CH9, RI9, RI10 (== CH10 sha), CUT7 (94372/6049/25313e39a1b344d7), R0, HALL... all match.
  - Not regenerated: R0V and IFB (need the real VS; reduce-rows takes no VS argument; R0V's FS is the original module so its sha is that of the original [I]). reduce-rows also emits GR*/RG* adaptive rows the device did not run.
- [V] spirv-val (--target-env vulkan1.2) passes on ORIG, IFC, CUT7, CH10, CH11, RI6, RI8 (dis/*.val empty, exit 0).
- Files: /tmp/ana-ch/rows/ (no F: files), /tmp/ana-ch/rows2/ (all), selected copies in /tmp/ana-ch/f/{ORIG,IFC,CUT7,CH10_3ff,CH11_7ff,RI6_7bf,RI8_6ff}.spv.

## 2. Structural diff CH10 vs CH11
- [V] The ONLY difference (`diff dis/CH10_3ff.dis dis/CH11_7ff.dis`): entry-function idx 1946-1950 (disassembly lines 4982-4986): `%7421..%7424 = OpLoad %v2float %7..%10` and `%7425 = OpLoad %v4float %11` become `OpCopyObject %v2float %317` (x4) and `OpCopyObject %v4float %284` (null constants). Same instruction count, same bound. Input vars: %7 loc2, %8 loc3, %9 loc4, %10 loc5 (v2float), %11 loc6 (v4float). FragCoord (%6) and PointCoord (%13) builtin loads (idx 1945, 1952, 1953) remain live in BOTH rows.
- [V] What the loads feed in CH10 (py/dead.py, taint.py):
  - loc2: 1 load (idx 1946) -> OpSelect (idx 2218). closure 55 instructions (taint closure incl. memory flow through Function variables), 2 OpSelect, 33 OpStore.
  - loc3: 1 load (idx 1947), NO user at all. closure 0.
  - loc4: load idx 1948 -> OpSelect (idx 2220). closure 53 (2 select, 31 store).
  - loc5: load idx 1949, NO user. closure 0.
  - loc6: load idx 1950 -> OpFConvert %v4half (idx 1951). closure 1 (the FConvert; its result is unused).
  - all five together: closure 108 of 3404 entry instructions (3.2%); 0 dynamic vector accesses, 0 OpAccessChain/OpPtrAccessChain with a dependent index, 0 OpImageSample, 0 OpImageRead, 0 conditional branches/switches; the dependent stores go to Function variables that are never read afterwards.
- [V] Dead code in the CH10/CH11 entry (py/dead.py, backward liveness from Output stores/terminators/calls; memory-aware for Function variables): of 513 non-structural entry instructions (excl. OpVariable/labels/branches/returns) 508 are dead, 5 live (loop counter OpLoad/OpIAdd/OpULessThan + 2 OpStore). Nulling the 5 loads makes no NEW code dead: it removes the 5 loads and 108 dependents of which almost all were already dead.
- [V] Reachability (py/cfg.py): CH10/CH11/RI6 entry has 473 blocks but only 5 are reachable from the entry label; ~105 non-OpVariable instructions are reachable (+1944 OpVariable in block 0). 425 OpReturn, 0 stores to Output variables. RI8 (no entry cut): 9990 entry inst, 469 blocks with code, only 6 reachable (the switch->default / one-trip loop rewrites cut the CFG), CUT7: 472 reachable blocks. ORIG: 473/473 reachable, a linear chain of 469 OpBranch.
- [V] Same structure on the ORIGINAL module (taint over the whole module, calls included; ORIG has 17041 instructions in function bodies, 832 OpVectorExtract/InsertDynamic, 641 OpSelect):

  | input | closure (inst) | dyn-vector ops with tainted operand | dyn-vector ops whose INDEX is tainted | AccessChain/PtrAC index tainted | OpImageSample* with tainted operand | ImageRead | cond branches | calls |
  |---|---|---|---|---|---|---|---|---|
  | loc2 | 6553 | 567 | 0 | 20 / 1 | 71 | 0 | 10 | 158 |
  | loc3 | 6335 | 567 | 0 | 20 / 1 | 60 | 0 | 10 | 132 |
  | loc4 | 6349 | 567 | 0 | 20 / 1 | 61 | 0 | 10 | 138 |
  | loc5 | 6335 | 567 | 0 | 20 / 1 | 60 | 0 | 10 | 132 |
  | loc6 | 2340 | 204 | 0 | 0 / 0 | 0 | 0 | 4 | 52 |
  | ALL | 6731 | 581 | 0 | 20 / 1 | 71 | 0 | 10 | 158 |

  So in the original ~39% of the body is input-dependent (data), but NO dynamic vector index (neither the 289 in the entry nor the 832 total; the matrix log says 761 of them are 64-bit) is computed from an input: they are driven by constants/SSBO data. [I] caveat: data-flow only (control dependence on input-dependent branches and pointer args of calls not followed). [V] model checked: memory flow via Function variables and call args/returns implemented.
- [I] In CH10 the live-input effect cannot be a data-flow/complexity effect (108 dependent instructions, all dead). What the nulling changes is that the entry reads 5 Location inputs vs 0: the driver sees 5 varyings fetched (even though unused) while the module has 1944 Function variables and 473 blocks.

## 3. OpImageRead presence (plain answer)
- CH10 (and CH11, CH9, RI8): ABSENT. 0 OpImageRead in the module; the two reads became `%7430/%7433 = OpCopyObject %v4float %284` (row text imageread->null=2) [V: dis/CH10_3ff.dis:4990].
- RI6: PRESENT, 2 OpImageRead (dis/RI6_7bf.dis:4999,5002), in the entry's first (reachable) block, entry idx 1955/1958; each result has 1 user (OpFConvert chain) whose result is dead (backward-liveness says dead). Not behind the entry cut. RI6 builds (BUILT) with them, but with inputs nulled. So reachable+present is true; unused-result is true [V]. CUT7 also has both (reachable) and FAILs with live inputs. The input attachments declarations: dropped in CH rows (input-attachment-decls/loads-dropped=4), restored in RI6.
- Consequence [I]: OpImageRead is not by itself sufficient to give -13 (RI6 BUILT) and not necessary (CH10 FAIL without it).

## 4. Metrics (entry = the only function except RI8/ORIG which keep 55 stubbed/real helpers)
Computed by py/an.py + py/reach.py ("linear live" = max number of SSA values live in linear instruction order, ignores loop back-edges and values held in Function variables: a LOWER bound for a real live-set; [I]).

| row | inst | blocks (total) | reachable blocks | Function vars | OpPtrAccessChain | OpAccessChain | dyn-vec ops | ImageRead | Sample | linear live max |
|---|---|---|---|---|---|---|---|---|---|---|
| IFC | 3043 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| CH11 | 6435 | 473 | 5 | 1944 | 0 | 40 | 0 | 0 | 0 | 19 (5 if reachable only) |
| CH10 | 6435 | 473 | 5 | 1944 | 0 | 40 | 0 | 0 | 0 | 19 (5) |
| RI8 | 13398 | 528 (473 entry + 55 helper stubs) | 6 | 1944 | 0 | 191 | 0 | 0 | 0 | 44 (5) |
| CUT7 | 6049 | 473 | 472 | 1944 | 0 | 32 | 0 | 2 | 0 | 7 |
| ORIG | 20189 | 723 | 473 (entry) | 3135 (1944 in entry) | 70 | 199 | 832 (289 in entry) | 2 | 72 (15 in entry) | 44 |
| RI6 (extra) | 6445 | 473 | 5 | 1944 | 0 | 40 | 0 | 2 | 0 | 19 |

Notes: [V] numbers; the global declaration section is ~3031 instructions in CH rows and is identical in IFC (IFC = those declarations + 4 entry instructions). So between IFC (BUILT) and CH11 (BUILT) the added static content is only the entry body (1944 vars, 473 blocks, ~3400 instr, 5 reachable).

## 5. CRASH exit=0 rows
- [V] Text origin: G7PipeRepro.java:67 (runLadder): `rowLine == null` -> `result=` + (ec[0]==-2 ? "TIMEOUT" : "CRASH") + ` exit=` + ec[0] + ` (no row line; process ended)`. rowLine is found only if one output line contains `"pipeline-reduce: row=" + id + " "` (line 66). The child is run per id with `--reduce --only <id>`, stdout+stderr merged (runOne, lines 91-101; redirectErrorStream line 92), timeout 40 s: timeout gives ec=-2 (line 99) -> TIMEOUT, so exit=0 is not a timeout [V].
- exit=0 means the child process exited normally with status 0: examples/pipeline-repro.c:31 `return r.reproduced ? 0 : 1;` and the repro itself reproduced (-13 as-is). A fatal signal would give a non-zero code [I: Java Process.exitValue on Android is 128+signal; not tested]. So: not a signal, not a timeout; stdout was not lost either (4 MB cap at line 64/95 irrelevant; the child prints other lines).
- [V] Real cause: the 10 rows are not executed at all. In pipeline_reduce.c:695-707 the `o` group walks FILE_NAMES (line 431: ssa, local-single-block, local-single-store, local-multi-store, scalar, inline, deadfn, dce, merge, deadbr, unify, strip, O, Os, locals, reenc, asm, hlsl, u2, u3, u5); `--list-rows` emits every id (line 699, list_only) so the ladder includes all 21 F: ids; but when `<fh>.red.<name>.fs.spv` is not found (lines 701-703) the code does `nmiss++ ... continue` without any `row=` line, and prints only the summary `"N offline file row(s) absent ..."` (line 707), which does not contain `row=F:<name> `. Java therefore labels it CRASH.
- [V] Set equality: the assets staged for fh ce5c70c189b74076 (assets/red) are red.{O, u2, u3, u5, locals, asm, unify, scalar, local-single-block, inline, ssa}; the 10 missing names are exactly local-single-store, local-multi-store, deadfn, dce, merge, deadbr, strip, Os, reenc, hlsl = the 10 CRASH rows. assets/red/MANIFEST.txt:3 says these were "omitted ... not shipped when unchanged or oversized". Regeneration with the assets dir reproduced the 11 present rows' shas exactly (section 1).
- Conclusion: the 10 are "file absent / not run" (should be n/a), NOT compiler crashes; the F:* result for these 10 spirv-opt variants is unknown (including the -O/-Os ones whose outputs are 9.5 MB). Minimal harness fix [I, design]: in Java line 67 treat `ec==0 && out contains "offline file row(s) absent"` as `result=n/a (file absent)`, or have pipeline_reduce.c:703 emit an `n/a` row (`row=F:x result=n/a`) via do_row(fs=NULL, why="file absent").

## 6. Proposed new rows (design only)
Existing evidence (device log) [V]: live inputs: CUT7..CUT354 (6049..17288 inst) all FAIL, original FAIL; CH10 (6435) FAIL; CH11 (6435, 5 loads null) BUILT; null column: RI8 13398 FAIL, FI-ALL (orig + null) FAIL; every single "restore one reduction" RI0-RI7, RI9 BUILT (all with inputs null, 6435); IFC/IFD/IFE (3043, inputs declared not loaded, 0 vars/1 block) BUILT; RI6 (ImageRead + IA) BUILT. So two facts: nulling is needed at 6435, and size is also needed (null fails at 13398). Confounders: all CUT/CH rows keep the same 1944 vars + 473 blocks; IFC drops all of it; CH rows differ from CUT rows in reachability (5 vs 472 reachable blocks); builtin inputs always live.

Rows (names new; mechanism: LRD_INPUT_CONST already has a per-input bit `input_sel`, spv_reduce.h; the cut parameter cut_blocks is free; the pruning and "IFC + loads" rows need two NEW rewrites: remove unreachable blocks/unused Function vars, and a MIN_ENTRY variant that loads Location inputs):
- Tier 1 (≈22 rows)
  - GRID on the CH base (all CH10 changes, cut level L, SSBO shrink on): L in {7, 59, 236} x {inputs live (L), inputs null (N)} = 6 rows; the full-size cell is known (CH8/CH9 live FAIL; RI8 null FAIL), and L=47 cell is CH10/CH11.
  - MINIMAL-SIZE live inputs: Q1 = IFC + 5 Location loads whose values are stored to the outputs; Q2 = IFC + 5 loads, results unused; Q3 = CH10 with unreachable blocks removed (keeps 1944 vars + 5 loads); Q4 = CH10 with unreachable blocks AND unused Function variables removed (≈ 3043 + ~100 inst, loads live); LD-DEAD = CH10 with loads kept but their uses replaced by null.
  - PER-INPUT: restore ONE input live in CH11: P2,P3,P4,P5,P6 (5 rows); leave ONE input null in CH10 (all others live): N2..N6 (5 rows).
  - VS control: CH10 and CH11 with the stub VS (2 rows; IFC vs IFD already say stub/real VS indifferent at small size).
- Tier 2 (if Tier 1 is not decisive): grid levels {29,118,354} x {L,N} (6 rows); cumulative restores ascending {2,3},{2,3,4},{2,3,4,5} and descending {6,5},{6,5,4},{6,5,4,3} (6 rows); CH10 with only builtin loads (FragCoord, PointCoord) -> null (1 row; expected FAIL); the 10 absent F: files (spirv-opt) so those rows really run.

Hypotheses and outcomes
- A = compiler complexity/resource limit that scales with static size AND number of live varyings (the driver does not DCE before the limit: CH10 is 98% dead, 5 reachable blocks, still fails).
  - Confirm: null column builds up to some size S_N (between 6435 and 13398) then fails; live column fails at a smaller size; Q1/Q2 (tiny + live loads) BUILT; Q3 FAIL or Q4 BUILT (static dead code matters); per-input results order-independent and count-like (P-rows singly BUILT, descending and ascending cumulative rows flip at the same count k*); the boundary moves down monotonically as more inputs are live; LD-DEAD behaves like CH10 (FAIL) (presence of the fetch, not dataflow, is what counts).
  - Refute: Q1 or Q2 FAIL at ~3043 (no size dependence); or a null cell FAIL at small L while a larger null cell BUILT (non-monotone); or Q4 FAIL (size irrelevant after pruning dead code).
- B = a specific interaction of one (or a few) live FS inputs with a construct.
  - Confirm: one location decides it: exactly one of N2..N6 turns CH10 BUILT (e.g. N6 flips: loc6 -> OpFConvert v4half, the only half convert on an input; or N2/N4: the OpSelect users) and exactly that P-row FAILs on CH11; the same single input also FAILs in Q1/Q2-style tiny rows; or LD-DEAD BUILT while CH10 FAIL (the data-flow/consumer matters, not the fetch). Also the builtin-only row FAIL would indicate "any input load" not Location-specific.
  - Refute: all five P-rows BUILT and all five N-rows FAIL (no single input matters), and no single input fails in tiny rows.
- C = something else (e.g. input-vs-VS linkage/interface, number of Function variables/blocks in combination with ANY varying, reachable-vs-unreachable code handling, 425 OpReturn, driver bug on this exact module).
  - Indicated when: CH10 stub-VS differs from real-VS result; Q3 FAIL but Q4 BUILT and the effect comes from the 1944 vars alone (i.e. var count x loads, not size in general); results in the grid are non-monotone or vary with how the code is cut at equal inst count; N-rows partially flip (e.g. loc3+loc5 only matter although their loads are dead).
- Combination reading of the existing data [I]: CH10 vs CH11 shows the live input loads are necessary at this size; RI8 vs CH11 shows size/static code is necessary; this already excludes "inputs alone" (B-single-cause without size) and "size alone" ; A or a size-dependent B/C remain.

## Addendum: Matrix log summary conflict (b9f7a0f5-attachment.txt, lines 106 vs 107)
- Line 106 `pipeline-matrix: summary pair ... built=0 failed=17 na=2` is printed by src/vulkan/pipeline_matrix.c:448-449 from the counters c->ok/fail/na of the -13 matrix (rows M0..M10). Its lists are right: BUILT=[] FAIL=[M0,M1,...,M9k] N/A=[M8,M8c] [V]. (M10 is `result=n/a` but emitted through line() at pipeline_matrix.c:442/ and is not in the N/A list/count: minor, na=2 means M8,M8c [V].)
- Line 107 `matrix: 2 built, 14 failed, 1 not applicable` is printed by src/vulkan/pipeline_repro.c:302: `rp(&rep, "matrix: %u built, %u failed, %u not applicable\n", c.ok, c.fail, c.na);` under `flags & LAGFX_PIPELINE_REPRO_MATRIX`. c is the pipeline_repro.c ctx_t (line 55-63) whose counters are bumped ONLY in variant() (lines 92-96: not applicable -> na++, VK_SUCCESS -> ok++, else fail++), which is called by run_matrix() = the OLD variant matrix with rows a,b,c1-c4,d0,d1,d2,e,f,g,h,i,j (log lines 60-83). The M0..M10 rows (lagfx_pipeline_matrix_run, line 296ff) use a different context and never touch c.ok/fail/na.
- [V] Reconstruction from the log's lines 61-83: OK = d0 (both stages stubbed) and d1 (real VS + stub FS) = 2 ("built"); FAIL = a, b x3, c1-c4, d2, e, g, h, i, j = 1+3+4+1+1+4 = 14; n/a = f = 1. Exactly 2/14/1. The 2 "built" are the by-design control rows d0/d1 (they use stub shaders, the message "meaning changed (stub stage)"). So the number is not wrong, but the label is misleading: it does not describe the -13 matrix, it includes the controls, and it is printed right after the M-matrix output with the same word "matrix".
- Other consumers [V by grep over /home/claude/henge and /tmp/wt-integ2]: `variants_ok/failed/na` are set at pipeline_repro.c:301, declared at include/libapplegfx-vulkan.h:475 ("matrix only: variants that built / failed / did not apply") and only read by tests/pipeline-repro-unit.c:169,200 (expects ok>0 on purpose). Android: G7PipeRepro.java does not read them; its verdict line comes from the process exit code in finish() and `ladder: ... built=` counts only `result=BUILT` ladder rows (line 69-73); `ladder-status` (line 79) uses R0/R0V only. So no other consumer uses the same count.
- Minimal fix [I]: change the text at pipeline_repro.c:302 to `variant matrix (a..j): %u built (includes the d0/d1 stub-stage controls), %u failed, %u not applicable` (and/or add the M-matrix result there from lagfx_matrix_result_t mr: mr.built/failed/na), no change to counters. The M10 n/a line could also be counted.

## Items for later [N]
- Not checked: real on-device behaviour of any proposed row; driver DCE behaviour; control-dependence taint; Android Process.exitValue signal encoding; IFB/R0V offline regeneration (needs VS argument).
