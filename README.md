<p align="center"><img src="docs/icon.png" width="120" alt="Henge"></p>

# Henge — Android port (experimental)

[한국어](README.ko.md) · [Previous Android README](docs/archive/README-android-2026-11-25.md) · [Original upstream README](docs/upstream/README.md) · [Upstream Henge](https://github.com/Leviidev/Henge)

Independent Android/ARM64 port of Leviidev/Henge, using Henge's ARM64 emulation engine rather than QEMU. Experimental; a complete macOS desktop boot is not established.

**Status: 2026-11-25.** Device: Samsung Galaxy S26 Ultra, Android 17, Adreno 840. Results below are developer-reported physical-device tests, not universal compatibility guarantees.

## Graphics compatibility library

[libapplegfx-vulkan](https://github.com/muns5965/libapplegfx-vulkan) ([tested revision](https://github.com/muns5965/libapplegfx-vulkan/tree/7574eac0d7396a9f49366f007c7fbeb44245826c)) is an experimentally verified Android ARM64 Metal AIR-to-SPIR-V translation and Vulkan graphics compatibility library. It is cross-built and exercised in the standalone Android GPU test APK; integration into the live Henge PVG backend remains unfinished.

[GPU test branch](https://github.com/muns5965/Henge/tree/research/gpt-vulkan-render-test) · [4.4 CI run](https://github.com/muns5965/Henge/actions/runs/45575251001) · [4.4 APK artifact](https://github.com/muns5965/Henge/actions/runs/45575251001/artifacts/15004136515)

## GPU compatibility matrix

PASS = observed success; FAIL = observed failure; NOT TESTED = no completed verification. Expected negative-control failures are not product failures.

| Test | Result | Evidence |
| --- | --- | --- |
| Android ARM64 build and launch | ✅ PASS | S26 Ultra |
| Vulkan dynamic dispatch | ✅ PASS | Native engine test |
| AIR triangle draw (stage 1) | ✅ PASS | 3,724 checked pixels, 0 mismatches |
| Texture sampling (stage 2, GLSL) | ✅ PASS | 4,096/4,096 |
| Alpha blending (stage 3, GLSL) | ✅ PASS | 4,096/4,096 |
| FP16 render and conversion (stage 4, GLSL) | ✅ PASS | 4,096 raw and 4,096 converted pixels |
| On-device AIR translation and draw (stage 5) | ✅ PASS | Host SHA-256 match; 3,724/3,724 |
| External engine clear/readback (stage 6) | ✅ PASS | 4,096/4,096 |
| ColorFill (stage 7) | ✅ PASS | 4,096/4,096; RGBA 00FF00FF |
| TextureCopy (stage 8) | ✅ PASS | 4,096/4,096; RGBA 4080FFFF |
| APK raw .bc packaging (4.2 / control B) | ❌ FAIL | Installation rejected |
| APK neutral .airdata packaging (4.3/4.4) | ✅ PASS | Installed and executed |
| ColorFill no-draw control | ❌ FAIL (expected) | 4,096 mismatches; exit 1; control PASS |
| TextureCopy no-draw control | ❌ FAIL (expected) | 4,096 mismatches; exit 1; control PASS |
| Real login vertex + fragment pair | ⬜ NOT TESTED | Reference GLSL vertex used for stages 7–8 |
| Patterned Metal texture / UV accuracy | ⬜ NOT TESTED | Stage 8 uses uniform color |
| UberComposite, Vfx, Xgc render | ⬜ NOT TESTED | Translation only |
| Live Henge PVG integration / macOS desktop | ⬜ NOT TESTED | Standalone GPU APK |

## macOS login shader corpus

11 AIR modules, 66 defined-function translations reported successful; every emitted SPIR-V file matched the Linux host SHA-256 and passed host Vulkan 1.2 spirv-val. These are translation/structural checks, not proof of full Metal semantics. The Xgc size is for its main function; its other 55 reported functions were translated separately, not independently GPU-executed.

| Function/module | Stage | Translation | Host hash / validation | Main SPIR-V bytes | GPU render |
| --- | --- | --- | --- | ---: | --- |
| ViewportToNDC | Vertex | 1/1 ✅ PASS | ✅ PASS | 2,056 | ⬜ NOT TESTED |
| UberCompositeVertex | Vertex | 1/1 ✅ PASS | ✅ PASS | 1,952 | ⬜ NOT TESTED |
| Vfx | Vertex | 1/1 ✅ PASS | ✅ PASS | 2,136 | ⬜ NOT TESTED |
| ColorFill | Fragment | 1/1 ✅ PASS | ✅ PASS | 900 | ✅ PASS |
| TextureCopy | Fragment | 1/1 ✅ PASS | ✅ PASS | 1,136 | ✅ PASS |
| UberCompositeFragment A | Fragment | 1/1 ✅ PASS | ✅ PASS | 1,544 | ⬜ NOT TESTED |
| UberCompositeFragment B | Fragment | 1/1 ✅ PASS | ✅ PASS | 1,524 | ⬜ NOT TESTED |
| VfxU11 | Vertex | 1/1 ✅ PASS | ✅ PASS | 2,940 | ⬜ NOT TESTED |
| TvcmXc_Isrc | Fragment | 1/1 ✅ PASS | ✅ PASS | 6,740 | ⬜ NOT TESTED |
| VfxXgb | Vertex | 1/1 ✅ PASS | ✅ PASS | 10,208 | ⬜ NOT TESTED |
| Xgc | Fragment | 56/56 ✅ PASS | ✅ PASS | 337,892 | ⬜ NOT TESTED |

Stages 7–8: fresh device translation, real fragment shader, GLSL reference vertex, 64×64 readback. Total 8,192/8,192 expected pixels; no-draw controls produced 8,192/8,192 mismatches.

## Android VM development

The main Android branch is [research/android-port](https://github.com/muns5965/Henge/tree/research/android-port). Android NDK builds, native runner, JIT/MMU adaptations, restore transport, sparse disk/AUX handling, clone boot, shared-memory preview and diagnostics are under development.

| VM milestone | Result |
| --- | --- |
| ASR system image transfer and verification | ✅ PASS |
| Cryptex and RecoveryOS installation | ✅ PASS |
| Boot policy and bootable-volume setup | ✅ PASS |
| System volume seal / complete restore | ⬜ NOT TESTED — completion not confirmed |
| Clone iBoot Stage 1 and Stage 2 | ✅ PASS (reported later clone run) |
| Normal macOS desktop boot | ⬜ NOT TESTED — no successful desktop session |
| Full Android display, input, audio, networking parity | ⬜ NOT TESTED |

Guest test: macOS 15.6.1 (24G90), VirtualMac2,1. Preserve original disk and AUX; use clones for writable boot tests. See [G7 notes](docs/g7-analysis-notes.md), [Android G7 workflow](.github/workflows/android-g7exec.yml), and [Actions](https://github.com/muns5965/Henge/actions). Artifacts expire and may require sign-in.

## Credits and licensing

Original Henge by [Leviidev](https://github.com/Leviidev). Henge: GPL-2.0-or-later; see [LICENSE](LICENSE) and [licensing notes](docs/licensing.md). External library attribution and licensing are separate; see its repository. macOS and Mac boot ROM belong to Apple. No claim of full Metal API or macOS desktop compatibility is made.
