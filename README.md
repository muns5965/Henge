<p align="center">
  <img src="docs/icon.png" width="120" height="120" alt="Henge">
</p>

# Henge — Android port (experimental)

An independent Android/ARM64 port of [Leviidev/Henge](https://github.com/Leviidev/Henge), originally developed for iPhone and iPad. This fork adapts Henge's own ARM64 emulation engine to Android; it is not a QEMU-based rewrite.

**Work in progress — currently being tested on a Samsung Galaxy S26 Ultra (Android 17). macOS desktop boot on Android has not yet been verified.**

[Original Henge README (archived)](docs/upstream/README.md) · [Upstream project](https://github.com/Leviidev/Henge) · [Android development branch](https://github.com/muns5965/Henge/tree/research/android-port) · [Builds / GitHub Actions](https://github.com/muns5965/Henge/actions)

## Changes from upstream

| Area | Android port |
| --- | --- |
| Build and runtime | Android NDK ARM64 builds, APK packaging, native runner processes and Android-specific platform integration |
| CPU and JIT | Adaptation of the existing ARM64 JIT/MMU engine to Android, including memfd-backed executable mappings |
| Restore | On-device G7 restore runner, IPSW SHA-256 verification, Android TLS transport and Apple signing-server retry diagnostics |
| Virtual devices | USB transfer error handling, queue/backpressure and cancellation improvements, reduced repetitive BDIF diagnostics |
| Storage | Sparse installation disk, AUX storage, incomplete-install preservation, storage management and verified sparse clones |
| Boot testing | Separate writable clone-only normal-boot runner, machine identity checks and selectable virtual USB diagnostic modes |
| Display | Shared-memory frame transport and a diagnostic frame preview; full Android desktop integration is not yet verified |
| Diagnostics | Runtime logs, memory sampling (RSS/PSS), process exit history, AUX comparison and iBoot environment-variable queries |

These are development changes, not a claim of feature parity with the original iOS app.

## Current device testing

Status as of 2026-11-25 (developer-reported device tests).

- Device: Samsung Galaxy S26 Ultra, Android 17.
- Guest: macOS Sequoia 15.6.1 (24G90), VirtualMac2,1.
- Experimental VM configuration: 2 GiB guest RAM, two vCPUs, 64 MiB JIT region, 128 GiB logical sparse disk.
- The restore reached ASR system image transfer and verification, Cryptex installation, RecoveryOS installation, boot policy generation and bootable-volume setup.
- The last observed restore stopped during `seal_system_volume`, before confirmed completion. The installation pair remains `in_progress`.
- Clone-based normal boot reached iBootStage1 recovery mode. USB port/enumeration variations did not resolve it.
- AUX inspection and live iBoot queries confirmed `auto-boot=false` and `boot-command=fsboot`. A clone-only auto-boot correction is being tested; persistence and subsequent normal boot are not yet confirmed.
- No successful Android macOS desktop session has been demonstrated.

## Experimental APKs

See [GitHub Actions](https://github.com/muns5965/Henge/actions) for Android probe and G7 execution builds on `research/android-port`. Build artifacts may require GitHub sign-in and may expire. The start-enabled G7 APK is an explicitly published test build, not a stable release.

Keep the original installation disk and AUX together. Do not delete app data or overwrite an incomplete installation while diagnosing it. Clone boot tests can write to the selected clone; they are not read-only tests of the original disk.

## Development

The Android port is developed on `research/android-port`. See the [Android G7 workflow](.github/workflows/android-g7exec.yml) for the runner and APK build steps, and [G7 analysis notes](docs/g7-analysis-notes.md) for diagnostic context. The original iOS build instructions and feature descriptions are preserved in the [archived README](docs/upstream/README.md).

## Known limitations

- Restore completion and normal macOS boot on Android remain unverified.
- Long-running restore stability and process termination causes are still under investigation.
- Full desktop display, input, networking, audio and upstream iOS feature parity are not established on Android.
- Current APKs are intended for development and testing, not daily use.

## Credits and licence

Original Henge by [Leviidev](https://github.com/Leviidev). This is an independent fork; upstream iOS results should not be interpreted as Android results.

GPL-2.0-or-later. See [LICENSE](LICENSE) and [licensing notes](docs/licensing.md). macOS and the Mac boot ROM belong to Apple and are not distributed as part of Henge.
