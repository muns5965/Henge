# S7 design: libapplegfx-vulkan inside the Android runner

Status 2026-11-25: design plus an UNBUILT glue file (`devices/pvg_lagfx.{h,c}`, compiled only with `-DHV_WITH_LAGFX`, listed in no build file,
called by nothing). The default VM behaviour is unchanged. Nothing here has run on a device.

## What the library really plugs into
`libapplegfx-vulkan` is a device, not a command back end. Its API is `lagfx_device_new` + `lagfx_mmio_read/write` + shell callbacks (create/map
task, read/write guest memory, raise interrupt). It decodes the same register layout as `devices/pvg_lite.c` (+0x1008 primary ring doorbell,
+0x1018 status, +0x1020 channel doorbell, +0x122c: checked in `src/doorbell.c`). So the connection point is the `gfx` MMIO window that
`hv_pvg_lite_create()` registers (`gfx_read`/`gfx_write` in `pvg_lite.c`), NOT the `HvPvgBackend` table (`create/submit/frame`), which sits one level
below pvg_lite's own ring and channel decoder. `HvPvgBackend` stays the right place only if the library's protocol layer were split out of its
device; that is not what it offers today.

## Wiring (when the pieces below are settled)
1. `hv_pvg_lite_create()`: if built with `HV_WITH_LAGFX` AND a runtime opt-in is set AND `hv_pvg_lagfx_create()` succeeds, register the gfx window with
   `hv_pvg_lagfx_read/write` instead of `gfx_read/gfx_write`. Otherwise the existing path, byte for byte. The iosurface window stays pvg_lite's.
2. Frames: `lagfx_display_new` + `lagfx_display_read_frame` give the host frame; it replaces `hv_pvg_lite_frame` / the PVG-surface read only
   while the library path is active. Display selection (boot_args vs PVG) is a separate item (reviewer D).
3. Android build: libapplegfx-vulkan as a static library built at API 34 (`.github/workflows/android-gpu-s4.yml` shows it compiles and has no
   `dlopen`), linked into the runner; the runner needs `-lvulkan`. On Android below 14 (API 34) the path is not offered.

## Open questions, in the order they can sink the plan
1. **Aliasing of guest memory.** Apple's `mapMemory` aliases guest pages into the GPU task. `lagfx_task_map_host_memory` copies. The glue copies too
   (comment in `cb_map_memory`). Correct for data the GPU only reads; wrong for GPU writes into guest memory and for guest changes after the copy.
   Real aliasing needs guest RAM as a shared (memfd) mapping. What the Android runner's guest RAM is now has to be read from `devices/vmapple.c`.
2. **Register-layout identity is assumed from the library source, not observed.** Run once with `LAGFX_LOG_LEVEL=trace` (log file in the app's
   files directory: Android has no `/tmp`) and compare the first doorbells with `HV_PVG_LITE_LOG` output of pvg_lite.
3. **Threading.** The library completes work on its own threads and calls `raise_interrupt`/`write_memory` from them. pvg_lite completes on the CPU
   that rang. `hv_gicv3_pend` and `hv_dma_written` from a foreign thread must be shown safe (device lock rules in `core/machine.h`).
4. **Guest speed.** The 2026-11-25 device runs show the guest nearly stopped after ~60 s (memory swapped out). Until a run shows the guest sending
   GPU work, the library cannot be observed doing anything. The foreground-service change (`G7RunService`) is the first thing to read in the next log.
5. **Licence (G0).** The library is MIT; Henge is GPL-2.0-or-later (compatible direction). The shader-translation projects are a separate question.

## Not done
No wiring in `pvg_lite.c`, no CMake, no runner link, no run. Do not read the S4 "compiled with the real Vulkan path" result as "the guest can draw".
