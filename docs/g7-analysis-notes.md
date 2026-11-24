# G7 analysis notes (2026-11-24) — code reading and public sources only; nothing here was measured on a device

## 1. AOSP phantom-process path (read from android.googlesource.com, main branch; the device's Android 17 build may differ)
- Kill chain for the CPU case: `MainHandler` message `CHECK_EXCESSIVE_POWER_USE_MSG` → `ActivityManagerService.checkExcessivePowerUsage()` → `updatePhantomProcessCpuTimeLPr(...)` → `PhantomProcessList.killPhantomProcessGroupLocked(app, proc, reason, subReason, msg)` with the reason `ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE` and a message of the form "excessive cpu ... during ... dur=...".
- Condition on the parent app seen in the same code: the app's set process state is `>= PROCESS_STATE_HOME` (not top / foreground-visible). A CPU percentage limit tier (`POWER_CHECK_MAX_CPU_1..4`) is compared with CPU time used over the check interval.
- `killPhantomProcessGroupLocked` kills the triggering phantom, then its siblings ("Caused by siling process"), then the PARENT APP ("Caused by child process"), unconditionally.
- A separate path, `trimPhantomProcessesIfNecessary`, kills only surplus phantoms above `MAX_PHANTOM_PROCESSES`; it does not kill the parent.
- The runner is started with `ProcessBuilder` from the app, so it is a child of the app process and a candidate for this tracking. WHETHER this killed the app in the observed run is NOT established: a kill by this path would normally show `REASON_EXCESSIVE_RESOURCE_USAGE` and the "Caused by child process" text in the exit record (`dumpsys activity exit-info`, logcat -b events/system). Without that record it stays one hypothesis next to memory pressure, a native crash and vendor policy.

## 2. USB / libusb shim / network buffers (host/libusb_shim.c, host/usb_host.c, devices/virtio_usb.c, host/dns_proxy.c)
- Per-transfer allocations in the shim are small: `XferBox` (transfer struct), control-transfer buffers of 8 + wLength (wLength ≤ 65535), descriptor copies; each has a matching free on the normal path (`libusb_free_transfer`, `free(buf)` in `ctrl_transfer`). Counted by grep: libusb_shim.c 12 allocation sites / 18 frees, usb_host.c 2 / 6, virtio_usb.c 4 / 8, dns_proxy.c 2 / 7. Counts are not a leak proof (error paths not traced one by one).
- Bulk data buffers are supplied by the CALLER (the restore stack / libirecovery code under third_party), not allocated by the shim. So whether firmware / IMG4 / IPSW members are held whole in memory is decided in that stack; this was not traced here and no peak is claimed.
- IPSW SHA-256 check (host/g7_ipsw_check.c) allocates nothing itself.
- Still open: trace the restore stack's buffers for the largest members (RootFS is streamed or not), Reverse Proxy session buffers, net_slirp queues. Needed before saying anything about USB/network memory.

## 3. Upstream Henge vs this port (diff -rq of core/ devices/ host/ against the read-only upstream clone; first-level ranking only)
- Differing files: core 10, devices 23, host 22; only in upstream: host/dt_fixup.{c,h}, host/snapshot.{c,h}; only in the port: 92 host files and 1 devices file (restore stack, G7 runner, Android runner, shared-memory frame path).
- Largest line differences (changed lines, both sides): devices/pvg_metal.m 596, pvg_lite.c 590, metallib.c 565, host/vm.c 547, devices/xhci.c 536, host/libusb_shim.c 316, host/install.c 262, host/usb_host.c 185, host/main.c 171, core/machine.c 171, host/vm.h 146, devices/bdif.c 135, host/jit_region.c 133, devices/virtio_usb.c 128, core/exception.c 110.
- Memory-relevant for the Android runs: host/vm.c (RAM and JIT allocation, HV_STAGE hooks), core/machine.c, host/jit_region.c (memfd JIT), host/disk.c (no F_NOCACHE on Android). Reading each of these file by file against upstream is NOT done; this is the list to read first.

## 2026-11-25 — why iBootStage1 stops in recovery mode (reviewer 6; source reading only, nothing on the device changed)

Restore-end handling (third_party/idevicerestore, libirecovery, host):
- dfu.c sends `setenv auto-boot false` + `saveenv` to iBootStage1 after the stage-1 components, then `setenvnp boot-args rd=md0 nand-enable-reformat=1 -progress ...`.
- recovery.c `recovery_set_autoboot` is called with 0 in `recovery_enter_restore`. idevicerestore.c sets auto-boot true + reset only for the old AppleTV path.
- No host code in this repo restores `auto-boot=true` / `boot-command` for a vmapple restore. That normalisation is expected from the restored ramdisk steps (set_one_time_boot_command, set_nvram_variables, persistent_boot_args, generate_boot_policies, bless_system_volume, set_boot_command, set_bootable all succeeded in the log) — whether they persisted into macos_aux.img is UNVERIFIED.
- The install pair was left `in_progress` with seal_system_volume not confirmed, so a skipped final switch to normal boot is possible but UNVERIFIED.

Read-only tools added for the next phone run (APK, mode D / AUX button):
- "AUX 비교 실행": macos_aux.initial vs macos_aux.img (size, hash, changed ranges, header, NVRAM-like regions, string hits flagged as a string search only), and each clone vs the original image.
- `--usb-mode 3` (D): mode C plus `getenv` of auto-boot, boot-command, boot-args, bootdelay, build-version using the same wire requests as irecv_getenv (vendor OUT 0x40/req 0 "getenv X", IN 0xC0/req 0). Only when the PID is 0x1280-0x1283; no setenv/saveenv/reboot/DFU/bulk. An empty reply means "unset or no answer" (not distinguishable); a failed transfer is logged as QUERY FAILED.
