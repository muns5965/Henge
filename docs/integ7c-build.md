# integ7c test build (research/g7-integ7c)

TEST build only. Nothing is published: no release, tag or prerelease; the workflow was dispatched with publish_start=false.

## Exact inputs
- Henge branch research/g7-integ7c, based on research/g7-integ7b 91eca80 (the SHA of the built commit is in the run page, see "Build record" below).
- Library libapplegfx-vulkan: see "Build record" (first pin attempt 448e3916e8ba12a85b43e8f89f8452e516c8af28 failed to compile with clang; the pinned commit is research/g7-integ7c-lib, 448e391 plus one clang atomic fix).
- Workflow: .github/workflows/android-g7lagfx.yml, with a research-branch-only step that uploads the signed g7start APK as the Actions artifact henge-g7-lagfx-test-apk (7 days).
- Baseline kept untouched: android-port 7cbc24b, library integ5 3ca2128, run 45954565563, versionCode 28535898.

## Build record
- Successful run: id 45984975578, run number 106, workflow Android G7 lagfx APK, https://github.com/muns5965/Henge/actions/runs/45984975578 (publish_start=false).
- Henge built commit: 9c3701009d01806ed6dbcc666a675b832f3000d1.
- Library pinned: 30ccf5e0efa4caaef119097d59aa6b5ab640e8a5 (research/g7-integ7c-lib = 448e3916e8ba12a85b43e8f89f8452e516c8af28 + the clang atomic fix in translate_function.c).
- APK: package dev.muns.hengeprobe, versionCode 28550809, versionName 0.2+9c37010 (baseline 28535898, 0.2+7cbc24b), 10,499,186 bytes.
- Signing certificate SHA-256 eb9cad5ef0acd5789690b84a31ed8320623a655b9590cda143642901f0141fec, identical to the baseline run 45954565563 (project keystore, not a temporary key): installs as an in-place update, app data is kept.
- Artifact: henge-g7-lagfx-test-apk (7,608,739 bytes zipped), kept 7 days.

## Main changes in integ7c
Library: exact MSAA default, whole-pass rejection, Blit Fill region/layout, atomics, named refusal of unlowered calls and int intrinsics, hold ledger, boot1 clock.
Henge: JIT size spinner, g7-jit line, fb0 default OFF, VBL_EXP incl. bit3_on_enable, NVRAM S0/S1 with backup, exact-stats checkbox + budget, GPU readback / WB verify checkboxes, full-resolution preview g7_frame.bin, VM halt on emit failure.

## Device checks
Follow docs/integ7-henge-runbook.md. First run: J0 (jit_mb 64, the default) versus J1 (256), everything else at its default, one change at a time.

## NOT expected to change
- QCOM tile shading: default OFF, the native path is not implemented.
- Gamma application.
- A2Xghfc -13: unsolved.
Judge a screenshot only on preview resolution and draw behaviour.

## What CI did not cover
No device run. The APK was built, linked and statically checked only.
