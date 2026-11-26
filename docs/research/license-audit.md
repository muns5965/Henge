# Provenance audit: libapplegfx-vulkan (MIT) vs medfa12/apple-silicon-anyos-project (GPL-2.0 top level, LGPL-3.0 vendor)

Not legal advice. Audited tree: /tmp/wt-integ2 at e8ab90d (clean, nothing modified). Reference: /tmp/ref/r at 59fd73a (GPL-2.0 LICENSE).
Upstream MIT: reachable, cloned to /tmp/ref/up, HEAD 7574eac0d7396a9f49366f007c7fbeb44245826c (2026-09-06), LICENSE = MIT, "Copyright (c) 2026 Matthew Jackson". Nothing copied from it.
Side effect: one `git log --all -S` I ran triggered git auto-gc (.git packing only; working tree untouched, status clean).

## Summary table

| File | Taken from ref | Own-dump evidence | Textual similarity | Risk |
|---|---|---|---|---|
| arm64_depth_stencil.h/.c | layout, bit fields, written extent, "unknown bits" analysis | none (header admits; no tag-4 object in either dump) | comment paraphrase of ref docs; field name `unknown_bits` ~ ref `unidentified_*_bits`; tests reuse ref fixture constants | MEDIUM |
| arm64_draw_decode.h/.c | all 14 record layouts, compact/wide scheme, written-extent "N of M", offset-before-count in 0x0b/0x0a | only tags 0x01 and 0x07 (16 draws in dumps) | "offset comes BEFORE the count here, opposite of..." near-verbatim idea/wording; code itself differs | MEDIUM |
| arm64_sampler.h/.c | bits 30:26 aniso, 25:24 mip, 21:20 border, 3-bit address modes, compare, flags | 6 samplers: state words 0x84000000 x3, 0x84000005, 0x04000000, 0x04000005 -> confirms bits 31/2/0 only | comment wording; tests use ref example 0x84255500 | LOW-MEDIUM |
| arm64_pipeline.h (+ .c, blend / vdesc) | only "key 2 = blendingEnabled, absent = off" | 43 descriptors (key list, values, absence) derived from own dump before ref was consulted | comment names ref + GPL + "tokens only" | LOW |
| arm64_resources.c | blit 0x13f layout (ref blit.rs) | none (not in dumps) | none (own vkCmdFillBuffer code) | LOW |
| arm64_exec_run.c | idea of "gather" and "snapshot of indirect args" | misaligned binds seen on device (desc unaligned=19) | none in code; comments cite ref | LOW |
| docs/arm64-opcode-comparison.md | whole opcode inventory, payload sizes, total lengths, statuses, ref file paths, 2 ref identifiers/phrases | only a few rows (tags 1,7,26,110..130,157/161/162, samplers) | structured derivative of ref catalogue | MEDIUM (highest of set) |

## Alternative independent source (important)
/home/claude/muns5965/henge/devices/pvg_metal.m (Henge, the replay host; LICENSE there is GPL-2.0 text) already decodes: sampler tag 3 (min/mag/address/border/mip at same bits, 4-bit-wide masks), depth-stencil tag 4 (depth byte bits 2:0/3, bits 4/5 treated as front/back stencil enable flags, ops bit fields identical, masks), draw tags 0,2,4,5,6,8,9,10/28,11/29,17 incl. offset-before-count for 0x0b. Library's first sampler decoder (before a452d27) was built from it ("pvg_metal.m makeSerialized"). So most facts are independently available, but (1) Henge is also GPL-2.0 per its LICENSE, so it is not a clean-room source for an MIT library either (authorship/licence of pvg_metal.m not established: unknown), and (2) Henge conflicts with ref on two points: tag 0x10 (Henge: ref u32@0, prim u16@4, offset u64@8; ours/ref: offset u64@0, ref u32@8, prim u16@12) and depth_state bits 5:4 (Henge: stencil-enable flags; ref and ours: unidentified). Our 0x10 layout therefore rests only on ref and is unverified by own data.

## Per-file notes

### arm64_depth_stencil.h / .c (commit 776e0d2, 2026-11-26, author "Claude")
(a) Single commit; message: "depth/stencil attachments, depth-stencil state objects..." (no ref mention in message). Header says "Evidence for the layout: the reference implementation medfa12/... ops/depth_stencil.rs ... only layout and field meanings taken, no code."
(b) Offsets (+0c, +10, +1c, 40 bytes), bit assignments (2:0 compare, 3 write, 5:3/8:6/11:9 ops), semantics (bits 5:4 unknown, 31:12 zero, bytes +0d..+0f unwritten/stale), acceptance rule ("same as the reference").
(c) Code: face() and decode are the obvious shifts; no identical identifiers (ours `depth_compare`, `fail_op`, `pass_op`; ref `compare_function`, `stencil_failure_operation`...). Comment/analysis overlap: "stale guest memory", "bits 5:4 written but unknown meaning", "ops bits 31:12 zero in every observed case" (ref: `unidentified_ops_bits`, "twenty bits ... no case has moved", "Read 0 in all 18 cases"), "0b11 in every observed record" (ref: "read 0b11 in every case"). Metal enum list in header is public API. Tests/arm64-depth-stencil-unit.c reuse ref's arbitrary fixture values (0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00; ops words 0x1f/0x107/0xa07/0x37/0x1c7/0x407; depth bytes 0x37/0x34/0x3f) and its header says so.
(d) No own evidence at all; "observed" in comments refers to ref's experiments. Henge supplies same layout independently (see above).
(e) MEDIUM. Remediation: reword header as own statement of the layout citing Metal enums; drop "observed" claims and ref-derived analysis of bits 5:4/31:12 (or mark as unverified); replace test constants with own values; ideally re-verify against a real tag-4 object from a dump; consider naming differently.

### arm64_draw_decode.h / .c (commit a452d27)
(a) Single commit; message: "layouts from the independent reference derivation (offset-before-count in 0x0a/0x0b, signed base vertex, 16/64-bit widening)". Header: "Source of the layouts: ... ops/render.rs, contract/draw.rs, used for protocol facts only."
(b) Opcode numbers 0x00-0x0b, 0x10, 0x11; field order/widths; wide-sibling rule ("every count wide"); written extents ("26 of 28", "14 of 16", etc. = ref's measured written lengths); i16 truncated base vertex; indirect arg blocks (public Metal structs).
(c) Code (switch over tags, macros L16/L32/L64, `need` table) is structurally ours; field names differ (prim/first_vertex/count vs primitive_type/vertex_start/vertex_count). Field order is forced by wire. Closest wording: ours "the offset comes BEFORE the count here, the opposite of 0x07/0x09" vs ref "**The offset comes before the count here**, which is the opposite of [DrawIndexed]..."; "truncated by the serializer" vs ref "serializer truncates it to 16 bits"; "wide form ... EVERY count wide" vs ref "widens every field". The `need` table equals ref's written-extent facts.
(d) Own dumps: draw_tag=1 (5+9 draws) and draw_tag=7 (11+7); nothing for 0x00,0x02-0x06,0x08-0x0b,0x10,0x11. Unit test decodes synthetic records. Henge covers most layouts independently but conflicts on 0x10.
(e) MEDIUM. Remediation: rewrite comments in own words from Metal API semantics; drop the ref pointer or keep a neutral note; mark non-dump-verified layouts "unverified"; settle 0x10 against own capture or Henge; written-extent numbers are the least independently derivable facts (consider using full record size / bounds only).

### arm64_sampler.h / .c (4f024ac, a452d27; predecessor make_sampler in arm64_exec_run.c from earlier commits)
(a) First decoder (pre-a452d27) was from Henge pvg_metal.m (4-bit address/border widths, wrong). a452d27 rewrote it "bit fields confirmed against the reference"; 4f024ac added unnormalized after seeing it in the own dump.
(b) Bit widths/positions (3-bit address, 2-bit border, 2-bit mip, 5-bit aniso verbatim, compare 3 bits), written extent (28 of 36), flags byte.
(c) No shared identifiers; header lists the same fields in the same order as ref docs, which is wire order. Tests (tests/arm64-draw-decode-unit.c lines ~67-104) comment "the baseline of the reference", "the reference's own example" and use 0x84000000/0x84255500/0x84000300/0x84004000/0x84020000 (ref's fixture values).
(d) Own dump: 6 samplers confirm only bit 31, bit 0, bit 2 (the doc says so). Everything else: ref (+ partly Henge).
(e) LOW-MEDIUM. Remediation: reword comments; relabel tests' constants as synthetic and remove "reference" wording; keep.

### arm64_pipeline.h (and arm64_pipeline.c blend / vertex descriptor)
(a) 4261294/0784ae9 derived blend keys 1..9 from the 43 device descriptors; 41bdb6d changed key 2 semantics after comparing with ref ("tokens only, no code taken"). ea60375 vertex descriptor grammar derived from device failures (message gives own evidence).
(b) Only the meaning of key 2 (blendingEnabled; absent = off).
(c) No code overlap; comment names the ref and its GPL. pipeline.c line ~110 says the grammar is "from the independent notes in the reference tree": I found no vertex-descriptor grammar notes in /tmp/ref/r (no .md/.rs describing it; spirv_vertex_input.rs is about SPIR-V). That attribution is unverified; the grammar appears to be own-dump-derived.
(d) Own dumps cover it (43 descriptors, 13 with factors but no key 2).
(e) LOW. Remediation: remove/neutralise the ref mention in pipeline.h and fix the pipeline.c comment so it only claims own dump evidence.

### arm64_resources.c (blit 0x13f at ~line 979; other changes own, e.g. mirror objects, swap write-back, capture)
(a) Introduced in a452d27/4f024ac ("layout from the reference derivation"); file's other 20+ commits are own-dump/own-design work.
(b) 0x13f record layout {ref u32, loc u64, len u64, pattern u32} (same as 0x132 plus widened pattern).
(c) Implementation (alignment check, vkCmdFillBuffer for 4-aligned middle, tail through staging) not in ref; comment cites "pvg-vulkan-wire ops/blit.rs".
(d) 0x13f not in dumps (only blit 300 x5).
(e) LOW. Remediation: drop file path from comment; mark layout unverified.

### arm64_exec_run.c (lines ~508, ~974 comments; larger file is own work)
(a) Comments added in a452d27 (gather of misaligned storage binds; snapshot of indirect args).
(b) Ideas only: gather-copy for misaligned binds, indirect arg snapshot semantics.
(c) No code/identifier similarity found; comments say "Reference: the independent pvg-vulkan runtime does the same ('gather')". Own dump shows `desc unaligned=19` as real motivating evidence.
(d) Yes for the need (device dump); no for ref-specific details.
(e) LOW. Remediation: delete the two ref mentions.

### docs/arm64-opcode-comparison.md (4f024ac)
(a) Single commit, message: "every render/compute/blit command of the reference vs. this path".
(b) Entire catalogue: opcode numbers, command names, payload sizes and total lengths for ~100 tags the library does not implement (patches, ICB, compute 0xc8..0xe6, heaps, rate maps, info queries 0x1c2..0x1d5), ref file paths, ref identifiers ("render_noop_barrier"), phrases ("lost work"), behaviours of the ref ("the reference does nothing either", "solves this by gathering").
(c) Table structure follows ref's module organisation (render/render_pass/tile/blit/compute...). Not code, but a systematic derivative of the GPL project's documentation; most rows have no own-dump backing.
(d) Own evidence only in the "Device evidence" and sampler sections (tag counts from a run, 6 sampler words). Also cites local-only paths (/tmp/zx).
(e) MEDIUM (highest). Remediation: remove from MIT tree (or keep outside the repo), or regenerate as a table of tags actually seen in own dumps / Henge with own status; strip ref identifiers and quotes.

### Other files mentioning the ref (grep of tree)
No hits for anyos/pvg-vulkan/metal2vulkan/medfa12 outside the seven above, plus: src/protocol/arm64_pipeline.c:110 ("independent notes in the reference tree"), tests/arm64-depth-stencil-unit.c:1, tests/arm64-draw-decode-unit.c:67,79 ("the reference"), commit messages (41bdb6d, a452d27, 4f024ac and their rebased twins 1353cf6, 11a257f, a464d23 on other branches). Commit messages stay in history; removal in files would not remove them (history rewrite would be needed if desired).

## Uncertainties
- Facts about wire formats (offsets, opcodes, bit positions) are likely uncopyrightable interoperability facts, but the ref's descriptions, experiments' selection, and tests/fixtures are expressive; GPL "derivative work" analysis depends on jurisdiction and was not done.
- I compared by reading; no automated clone detection beyond identifier/phrase checks. Not all of ref (2474-line render.rs, etc.) was read in full.
- Henge pvg_metal.m licence/authorship not established; same concern applies to README's "follows the semantics of Henge" and arm64_objects.h "Reference: Apple's devices/pvg_metal.m".
- Whether the user is the sole author of this fork's contributions (all commits authored as "Claude") is outside scope.
