# L8 provenance report: arm64_sampler.h:11 and neighbouring sampler code

Read-only. Library /tmp/wt-integ2 (branch research/g7-integ2, HEAD ece74ae, tree clean). Nothing in the repo changed. I did not open /tmp/ref/r, medfa12/*, reims-vgpu, steelbrain/* or dxmt. All statements about the external project come from OUR commit messages, comments, docs and the earlier audit notes. Not legal advice.

## 1. History (git log --follow / blame)

| What | Commit(s) | Date, author | What the message says |
|---|---|---|---|
| arm64 tag-3 sampler decode, first form: `make_sampler` in arm64_exec_run.c | fa2d54e (pipeline-state resolver, descriptor binding...) | 2026-11-26, "Claude" | No mention of a source in the message. Code reads address/border as 4-bit fields, no aniso. Per license-audit.md it was built from Henge `devices/pvg_metal.m` (the code matches that block line for line in field positions). |
| arm64_sampler.h/.c created (one file pair, `git log --follow` shows no earlier name) | a452d27 (1353cf6 is the same commit before a history rewrite) | 2026-11-26 18:42 +0900, "Claude" | "tag-3 sampler decode (bit fields confirmed against the reference): address mode 3 bits, border only with a border-clamp axis, ... aniso ...". Line 11-12 of the header (the "Source of the bit assignments" text) was introduced in this commit and is unchanged since (git blame: lines 1-15 a452d27, later lines 4f024ac / 9ea8fc2). |
| unnormalized-coordinate mapping | 4f024ac | 2026-11-26 18:45 | "unnormalized coordinates (present on the real device: 2 of 6 captured samplers)". Also adds docs/arm64-opcode-comparison.md. |
| lagfx_a64_sampler_decode_ex, mirror-clamp-exact | 9ea8fc2 | 2026-11-27 | No source mention. |
| test of the decoder (tests/arm64-draw-decode-unit.c, test_sampler) | a452d27 | same | Test comments: "the baseline of the reference", "the reference's own example". |
| constexpr_sampler.{h,c} + tests/air2spv-constexpr-sampler-unit.c | 69dce33 (first, only normalized bit + LOD clamps), f0bd3cb (address/filter/mip/pixel fields, corrects 508dad5) | 2026-11-27, "Claude" | f0bd3cb: "Layout is external (two published sources) and unverified on device". Header names them: an MIT project that derived the layout from Apple's compiler output, and Apache-2.0 LuisaCompute. |
| sampler_cache, air2spv-sampler-lavapipe.c, gen-sampler-fixtures.sh | fc215ac, fdff7f7, 9ea8fc2, 0ffa4e8 | 2026-11-27 | Fixtures are hand-written AIR (header says "not Apple output"); the expected pixels come from an independent CPU reference (word "reference" there means the CPU model, not the external repo). |

Author field is always "Claude <noreply@anthropic.com>" (agent sessions); no human author differentiation exists.

grep for `medfa12|anyos|reims|steelbrain|dxmt|pvg-vulkan-wire` over src tests tools docs headers (HEAD ece74ae):

| Hit | Text |
|---|---|
| src/protocol/arm64_sampler.h:11 | "Source of the bit assignments: the independent derivation in medfa12/apple-silicon-anyos-project (pvg-vulkan-wire ops/sampler.rs), which agrees with the previous reading of pvg_metal.m makeSerialized in this file." (Note: "in this file" is stale; the pvg_metal.m reading was in arm64_exec_run.c.) |
| src/protocol/arm64_depth_stencil.h:15 | "Evidence for the layout: the reference implementation medfa12/... ops/depth_stencil.rs ..." |
| src/protocol/arm64_draw_decode.h:29 | "Source of the layouts: ... ops/render.rs, contract/draw.rs" |
| src/protocol/arm64_pipeline.h:45-46 | "matches an independent implementation ... (medfa12/..., GPL: only the token meanings were compared, no code taken)" |
| src/protocol/arm64_resources.c:1004 | "... pvg-vulkan-wire ops/blit.rs" |
| docs/arm64-opcode-comparison.md:6-7, 114-117 | whole reference path list; "Reference sampler record (`ops/sampler.rs`, total 36 bytes, 28 written ...)" |
| constexpr_sampler.* , sampler_cache.*, air2spv-*sampler* | NO hit. Their stated sources are the two MIT/Apache projects (oss-sampler-bits.md sources A, B), not the GPL repo. |

## 2. Classification

| File / item | Class | Reasoning |
|---|---|---|
| arm64_sampler.h:11 comment | (a) bare attribution, with a (b) claim inside ("agrees with" is true in substance; "Source of" is the overstatement) | Names the repo and the Rust file path. |
| arm64_sampler.h bit list [0] min, [2] mag, [4] lodAverage, [7:5] compare, [10:8]/[14:12]/[18:16] S/T/R, [21:20] border, [25:24] mip, [30:26] aniso, [31] normalized | (b) for every field POSITION: also present in our own first decoder (fa2d54e, from Henge pvg_metal.m) before the external repo was consulted. Widths (3-bit address, 2-bit border) and "aniso verbatim 1..16" = (d)/(b'): taken from the external derivation (commit msg "bit fields confirmed against the reference"), but forced by the Metal enum ranges (see section 3). | Wire order of fields is dictated by the format; list order in the header equals bit order. |
| arm64_sampler.h payload sentence "36 bytes allocated, first 28 written, +3 bytes unwritten, 8 bytes never written" | (d) unclear | The "written extent" numbers are measured by the external derivation; our docs state 36-byte payloads in our own dump (doc line 108 "all 36 bytes") but "28 written" is not shown to be ours. |
| arm64_sampler.c | (b)/(c-low) | License-audit: no shared identifiers (ours `lagfx_a64_sampler_decode`, flags `LAGFX_A64_SAMP_*`). Structure is plain shift/mask. Flag names are own. |
| Flags/mapping policy (unnormalized restrictions, lodAverage, compare unmapped, mirror clamp) | own work | Derived from Vulkan rules and own device dump ("2 of 6 unnormalized"). |
| tests/arm64-draw-decode-unit.c test_sampler, lines 67 and 79 comments + values 0x84255500, 0x84155500, 0x84000300, 0x84004000, 0x84020000 | (c)-lite | Comments say "the baseline of the reference" and "the reference's own example", i.e. an admission that the fixture value came from the external project. The values are trivially constructible from the layout (0x84255500 = 5 in each 3-bit address field, border 2, normalized, aniso 1), so low expression risk, but the comment is the problem. |
| docs/arm64-opcode-comparison.md:114-130 (sampler record paragraph) | (c) | A paraphrase of the external record description (offsets, "bits 7:4 and +9..+11 unwritten", "supportArgumentBuffers", written length), with its file name. Audit rated this doc the highest risk (MEDIUM). |
| constexpr_sampler.h/.c, constexpr unit test (4 captured words), sampler_cache, lavapipe tests, fixtures | not derived from the GPL repo | Sources A (MIT, oosexgang/hakios-metal, states Apple-binary provenance) and B (Apache-2.0 LuisaCompute, "observed Apple compiler output via MSL oracle") per oss-sampler-bits.md. Different layout (64-bit AIR word) from the 32-bit runtime state; no overlap with the GPL file's layout. |

Conclusion on (c): no code or identifier copying found in arm64_sampler.{h,c}. What exists is (1) attribution wording that overstates "source", (2) test comments/fixtures that cite the external repo, (3) a docs paragraph that paraphrases the external record description. Items (d): widths/written extent/flags byte.

## 3. Independent evidence we have for the runtime sampler word

Our own captured data (docs/arm64-opcode-comparison.md:108; commit 4f024ac; run 20261126-145344, log in /tmp/zx; the six payload words themselves are NOT stored as files in /tmp/zx/dump, only pso_*/lib_* are, so I could not re-extract them): six 36-byte samplers, state words 0x84000000 (x3), 0x84000005, 0x04000000, 0x04000005.

What these prove (by themselves, no external source):
- bit 31 differs between 0x84.. and 0x04.. and goes with "unnormalized" behaviour only if the guest used pixel coordinates (that interpretation is itself from Henge / the external repo; the dump shows only that one bit toggles). So "bit 31 = normalized" is consistent with the data, not proven by the dump.
- bits 0 and 2 occur together or both clear. They prove two bits exist that look like filters, but because they never occur apart, they do NOT prove which is min and which is mag.
- bit 26 (0x04000000) is set in all six words and bits 27-30 are 0: consistent with "aniso field verbatim, value 1", but the field position is not proven by this.
- bits 3-30 otherwise zero in all six: address mode = 0 (clamp to edge), border 0, mip filter 0, compare 0: consistent only.
Not proven by any of our dumps: min vs mag bit assignment, lodAverage (bit 4), compare (7:5), address mode position/width (bits 8-19 are always zero), border colour (21:20), mip filter (25:24), aniso position/width, non-trivial address enum values.

Henge `devices/pvg_metal.m` lines 492-512 (our repo copy, written before the external repo was consulted in this library): decodes tag 3 with the same field positions (min bits 1:0, mag 3:2, lodAverage 4, compare 7:5, S/T/R at 8/12/16, border 20, mip 25:24, aniso 30:26, normalized 31, argument buffers bit 32). It feeds MTLSamplerDescriptor on a real Metal host, i.e. it is a functional decoder, but (i) commit 88f4900 is a squash ("Build 5.3 APK...") and no authorship/validation evidence for that block, (ii) the repo LICENSE is GPL-2.0 text. So it is independent of medfa12 as a source of facts, but not clean-room for MIT purposes and not verified. Its 4-bit widths for address/border are a superset of the 3/2-bit widths; the difference matters only if the unused bits (11, 15, 19, 23:22) are ever set.

Facts that rest ONLY on the external derivation (plus Henge's agreement on positions):
1. 3-bit address width and 2-bit border width (Henge uses 4). Forced anyway: MTLSamplerAddressMode has 6 values (0..5) so 3 bits; MTLSamplerBorderColor 0..2 so 2 bits; public Apple enums.
2. "28 of 36 bytes written", written-extent table, LOD floats at +12/+16 of the object.
3. Which of bit 0/bit 2 is min/mag (Henge agrees; external agrees; our data cannot discriminate).
4. flags byte semantics (supportArgumentBuffers).
5. Anything about bits 3, 11, 15, 19, 23:22 being unused.

Constexpr word (64-bit AIR constant, separate format): four captured words in tests/air2spv-constexpr-sampler-unit.c (0x807bff0000088a00, ...88000, ...80092, ...80a49), taken from the AIR in our dumps (/tmp/x231501/dump etc.). Per oss-sampler-bits.md, they decode consistently with sources A+B; B's own differential oracle independently confirms bits 0-8 address, 9-12 filters, 13-14 mip, 20-23 aniso, 24-55 LOD. Our four words prove: address 0/1/2 values at bits 0..8 (0x92 = 2,2,2; 0x49 = 1,1,1; 0x00 = 0), bit 9 / 11 (0xa00) linear pair, bit 15 set in two words, bit 19 (compare=8) set in all, bit 63 set in all, LOD field 0x7bff at 40-55. They do not prove: mag vs min split (always both set), mip filter bits 13-14, aniso 20-23 (always 0 field), compare enum, border bits.

## 4. What the earlier audit concluded

- docs/research/license-audit.md (Henge repo) did cover arm64_sampler.h/.c (audited at e8ab90d, before ece74ae): rating LOW-MEDIUM. Found: bit widths/positions from the reference; own dump confirms only bits 31/2/0; test constants reuse the reference's examples (0x84255500); comment wording; remediation = "reword comments; relabel tests' constants as synthetic and remove 'reference' wording; keep". It also notes the first decoder came from Henge pvg_metal.m. It did NOT cover constexpr_sampler.* (which uses other sources) and did not evaluate the stale "in this file" claim. It did not state that Henge independently supplies the field positions; it did (Alternative independent source section), including that Henge is GPL-2.0 text.
- oss-sampler-bits.md covers only the 64-bit constexpr word and sample-operand order, not the tag-3 runtime state word. Its provenance caveat for source A (derived from Apple binaries; licence ok, provenance not clean) applies to constexpr_sampler.h and was recorded; the header says so only partially ("states it derived them from Apple's compiler output").
- followup-ledger.md L8 asks exactly this question; "comment deletion alone is NOT a fix" (approved).

## 5. Recommendations

### 5.1 Header comment (arm64_sampler.h:11-12), replacement text

Replace the two lines with:

```
 * Field positions: our own decoder of this payload in Henge's replay path (pvg_metal.m, a reading of the guest data written by the Apple stack; positions
 * unverified against Apple documentation) and an independent third-party derivation of the same serialization were compared: they agree on every position
 * below. Facts proven by our own captures: bit 31 toggles between the observed words, bits 0 and 2 are set in the observed filter-linear words and bit 26
 * is set (aniso field value 1) in all six captured samplers (see docs/arm64-opcode-comparison.md, device evidence). Everything else (min vs mag bit,
 * address / border / mip / compare / aniso positions, written extent) is INFERRED: widths follow the public MTLSamplerAddressMode / MTLSamplerBorderColor /
 * MTLCompareFunction / MTLSamplerMipFilter enum ranges, positions follow the agreement above. No code or text was taken from either source.
```
(Optionally keep the third-party repo name out of an MIT header and put it in a NOTICE/provenance doc; if policy wants the name, "agrees with medfa12/apple-silicon-anyos-project (GPL-2.0), compared as protocol facts only" is the minimal truthful wording.) Also fix the stale "in this file".

### 5.2 Other wording edits (facts + "agrees with")
- tests/arm64-draw-decode-unit.c:67 -> "baseline: nearest, clamp to edge, not mip mapped, normalized (matches the captured 0x84000000)"; line 79 -> "synthetic: border opaque white, S=T=R=clamp to border (value built from the field positions, not from a capture)". Lines 73-104: label values as synthetic; keep only 0x84000000/0x84000005/0x04000000/0x04000005 as "captured".
- docs/arm64-opcode-comparison.md:114-117: rewrite the record layout paragraph as our own statement ("our dump: 36-byte payloads; the layout below is inferred and agrees with the external derivation") and mark written-extent numbers (28 of 36) as external/unverified. Drop file path names or keep one neutral "protocol facts compared" sentence (already in line 6 of the doc).
- constexpr_sampler.h header: keep, but say "bit 63, aniso, compare, border undocumented/unverified" explicitly (it mostly does) and add the A source's disassembly provenance.

### 5.3 Fields needing independent re-derivation before release (priority order)
1. min vs mag filter bit (0 vs 2): not decidable from the six words. Test: replay one guest capture where the app creates a sampler with min != mag, or ask Apple's AGX sampler descriptor behaviour through our own Metal-side test (Henge host: create MTLSamplerDescriptor with min linear / mag nearest and dump the serialized payload via the guest-side encoder). Only our dumps/Apple public API count.
2. Address mode positions/width (bits 8-19) and border (21:20): build-time evidence from Apple public enums (6 and 3 values) gives widths; positions need a capture with a non-zero address mode or border.
3. Mip filter (25:24), aniso (30:26), compare (7:5), lodAverage (4), flags byte: same approach, one capture each with a distinctive MTLSamplerDescriptor.
4. Written extent (28 of 36): measure from our own dump (compare 36-byte payload with zero-filled vs stale bytes across several samplers) or stop asserting it; decoders only need "n >= 20".
5. Constexpr word: mip (13-14), aniso (20-23), bit 19 default 8, compare, border/reduction: derive from our own AIR dumps with additional MSL oracle samples (B did this); otherwise leave documented as external/unverified (already stated).

### 5.4 Minimal clean-room plan (since (c)/(d) items exist)
1. Freeze: no further reads of the external repo for sampler work. A person/agent that has not read it writes an independent spec section from (i) Apple public Metal enums/headers (MTLSamplerDescriptor, enum ranges), (ii) our own dumps (six runtime words, four AIR words), (iii) targeted new captures on the Henge host with a hand-made Metal client that creates samplers with one varied property at a time (differential method, as B did), (iv) Henge pvg_metal.m only for position cross-check if its authorship is confirmed as the project owner's own work (check git history/upstream before relying on it; LICENSE says GPL-2.0).
2. Write the new field table + tests from that spec, with synthetic test values labelled synthetic and captured values labelled captured; compare it to the current header. Where they agree: the reworded comment of 5.1 stands. Where they differ: fix the code.
3. Rewrite docs/arm64-opcode-comparison.md sampler paragraph from the same spec, in own words.
4. Record in docs/research (provenance log): date, who/what, inputs used, diff versus the external derivation. Re-run the grep above in CI-like fashion to ensure no external-repo path remains in headers.
5. Decide policy for the other four files naming the repo (depth_stencil, draw_decode, pipeline, resources): same 5.1 pattern, and depth_stencil/draw_decode test constants (audit rated MEDIUM) need the same relabelling.

## Verified / inferred / not checked

Verified (read in this session):
- First introduction commits, dates, authors, messages listed in section 1 (git log --follow, git blame, commit bodies).
- The only external-repo mention in the sampler files is arm64_sampler.h:11 (comment), tests/arm64-draw-decode-unit.c:67/79 (wording "the reference", values), docs/arm64-opcode-comparison.md:6-7/114-130; constexpr_sampler.* has no GPL-repo mention.
- Henge pvg_metal.m lines 492-512 field positions (read); LICENSE text is GPL-2.0; sole commit 88f4900 (squash) so authorship of the block is not traceable by git.
- First library decoder (fa2d54e make_sampler) used 4-bit address/border widths and no aniso (read).
- license-audit.md and oss-sampler-bits.md contents as summarised.
- No code/identifier overlap beyond obvious shifts in arm64_sampler.c (header-level and test-level read; I did not diff against the external repo, by instruction).

Inferred:
- That fixture value 0x84255500 came from the external project (from the test comment saying so, and the audit).
- That the six sampler words are accurately quoted in docs (they match commit 4f024ac and the doc and tests; not re-extracted).
- Which fields are "forced" by public enum ranges (my reasoning from Apple's published Metal enums).
- Interpretations of what bit 31 means given the dump (consistent, not proven).

Not checked:
- The external repo's actual contents/licence terms for ops/sampler.rs (forbidden); therefore no similarity comparison and no statement about expression overlap beyond what our own comments/audit say.
- Upstream authorship of Henge pvg_metal.m sampler block, and whether Henge's code derived from the external repo.
- The six runtime sampler payloads in /tmp/zx (not stored as files there) and the later runs' dumps (/tmp/x231501, /tmp/x230109) for sampler words.
- Whether tests/arm64-exec-ext-unit.c and arm64-exec-res-unit.c contain further "reference" wording; only grep for the repo names/identifiers was run over them.
- Legal assessment of the MIT/Apache sources A/B (done earlier in oss-sampler-bits.md).
