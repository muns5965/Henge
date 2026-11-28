# Turnip / lavapipe / validation / diagnostics feasibility (unrooted Galaxy S26 Ultra, Adreno 840, Android 17)
Date: 2026-11-27. Research only. Nothing downloaded or executed from third parties; no clones left (Mesa clone failed: gitlab blocked by proxy; /tmp/mesa-src removed).
Legend: READ = seen in a source this session. INFER = my reasoning. UNVERIFIED = could not check.

## 0. Local facts (READ from our own files)
- /tmp/x231501/logs/g7_lagfx_1795702501625.txt: phys device 'Adreno (TM) 840' integrated-GPU, api=1.4.295 (READ).
- Device logs only contain 3 extension names (dynamic_rendering, dynamic_rendering_local_read, rasterization_order_attachment_access) in pipefail_*/state; NO full extension list, and no mention of pipeline_executable_properties or pipeline_creation_feedback anywhere in /tmp/x231501 or /tmp/x230109 (grep). So "does Adreno 840 expose VK_KHR_pipeline_executable_properties" = UNVERIFIED (INFER: Qualcomm's proprietary driver generally does not; unknown for 840). Cheapest check: add vkEnumerateDeviceExtensionProperties dump to the repro report (see section 4).
- Our library already has: optional VK_LAYER_KHRONOS_validation via env LAGFX_VK_VALIDATION=1 and VK_EXT_debug_utils messenger (src/vulkan/instance.c in /tmp/wt-lintex, lines ~225-340); instance apiVersion 1.3. The repro example (examples/pipeline-repro.c, MIT) calls lagfx_pipeline_repro_run -> uses that init. So validation messages already have a code path; only layer discovery on Android is open.

## 1. Mesa Turnip on Adreno 8xx
READ:
- Phoronix, article dated 20 Jan 2026: Adreno Gen 8 Vulkan merged into Turnip for Mesa 26.0; names X2 Elite "Glymur" and 8 Elite Gen 5 "Kaanapali"; December 2025 merge added initial Gen8 + freedreno GL; VRS disabled on gen8; mentions kernel MSM driver (Linux 6.19), not KGSL. https://www.phoronix.com/news/Mesa-26.0-Adreno-Gen-8-Graphics
- Mesa 26.0.0 release notes (released 2026-02-11, Vulkan 1.4 API): Turnip items seen: robustness2, multisampled_render_to_single_sampled, custom_resolve, pipeline_executable_properties loop count commit; page truncated, gen8 not seen in the first 100k chars. https://docs.mesa3d.org/relnotes/26.0.0
- Android Authority, 20 Nov 2025: Rob Clark (Qualcomm) said driver would support Adreno 840; 830 suggested. https://www.androidauthority.com/snapdragon-8-elite-8-elite-gen-5-devices-emulation-driver-3617718
- Mesa 24.3 notes (mirror): "tu: Expose VK_KHR_dynamic_rendering_local_read" (Connor Abbott) => extension exists in Turnip since 24.3 for gens where implemented. https://fuchsia.googlesource.com/third_party/mesa/+/main/docs/relnotes/24.3.0.rst
- VK_EXT_rasterization_order_attachment_access: Turnip MRs exist (https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/15106 ARM variant; /18420 "Trivially expose EXT") (titles only, via search).
- Third-party build page (Banners-Turnip v26.3.0-20260916, untrusted, licence not stated): says Adreno 840 support is already in Mesa main, A8xx ZIP built from whitebelyash 'turnip/gen8' stack is "Still experimental ... compile-verified only, not yet device-proven"; lists "UBWC 5/6 KGSL" among main features. https://newreleases.io/project/github/The412Banner/Banners-Turnip/release/v26.3.0-20260916
- Mesa docs: Turnip is the freedreno Vulkan driver, TU_DEBUG options (sysmem, gmem, nobin, noubwc, nolrz, rast_order...), FD_RD_DUMP "currently only integrated in MSM backend" (so not on KGSL), IR3_SHADER_DEBUG. https://docs.mesa3d.org/drivers/freedreno.html
- Mesa Android build doc: NDK cross file, `-Dplatforms=android -Dplatform-sdk-version=34 -Dandroid-stub=true -Dandroid-libbacktrace=disabled -Degl=disabled -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl`; official path for stock Android replaces the driver in /vendor (needs root/remount) – not what we want. https://docs.mesa3d.org/android.html
UNVERIFIED (could not read Mesa source; gitlab raw is robots-blocked and git clone 403 via proxy):
- Whether the gen8 + KGSL combination actually works on a retail 840 (KGSL UAPI differences for A8xx, slice/"sliced GPU" support), which exact Mesa release first works, and whether dynamic_rendering_local_read / rasterization_order_attachment_access are advertised on gen8 (gen8 may gate them; INFER: likely exposed if gen7 code is shared, but fail-closed possible). Must be checked on-device by running vulkaninfo-equivalent (our own dump).
- Mesa 26.0 notes beyond char 100000 (gen8 line probably there).

Verdict: Turnip gen8 code EXISTS upstream (>= 26.0, Feb 2026); working on retail 840 via KGSL = "needs work / unproven" (only third-party claims of "experimental").

### Running without root
READ:
- libadrenotools: BSD-2-Clause; "loading custom GPU drivers (Turnip example), BCn support, file redirection"; Android 9+, arm64, no root required. (mirror README: https://gitee.com/yuzu-mirror/libadrenotools, last update 2025-02-02). Used by Winlator / emulators (INFER from search results; not read here).
- Android Vulkan docs: system loader loads ONE HAL driver from /vendor/lib64/hw/vulkan.<hw>.so (or APEX from Android 15); layers for non-debuggable apps only from the app's native lib dir. https://source.android.com/docs/core/graphics/implement-vulkan
- GPU syscall filtering (Android 16 QPR2): SELinux xperm filter on GPU driver ioctls for appdomain apps; ioctls classed unprivileged/restricted/instrumentation; documented for Pixel 6-9 Mali; page does not mention KGSL/Adreno. https://source.android.com/docs/security/features/gpu-syscall-filtering ; https://blog.google/security/further-hardening-android-gpus/
Options:
(a) Direct ICD load from the repro tool (INFER; best fit): dlopen("<nativeLibDir>/libvulkan_freedreno.so"), take vk_icdGetInstanceProcAddr, and use it as the dispatch root instead of linking libvulkan. Needs: our library must support a "vkGetInstanceProcAddr override" (currently links vulkan loader directly via vkCreateInstance, so work needed: either load via function table or link against our own tiny trampoline). Turnip's .so must be built with -Dplatforms=android and an Android-compatible dependency set (libz, liblog, libnativewindow; android-stub for ANativeWindow/gralloc). Because we only do offscreen pipeline creation, WSI is not needed (INFER).
   Caveat: dlopen of an exec'd child process: the exec'd process uses the default linker namespace config for that path; app-private dir libs normally dlopen fine for the app's own files when the file is in nativeLibDir (that is how libg7pipelinerepro.so itself runs) (INFER; confirm on device). Turnip depends only on NDK-public libs if built right (INFER).
(b) Custom loader build: unnecessary for (a); a Khronos-loader build with ICD path env (VK_DRIVER_FILES / VK_ICD_FILENAMES) is plausible for an exec'd child since it is not in the app classloader namespace, but the NDK loader build for Android in an app-local dir is unusual (INFER; low value).
(c) libadrenotools: designed for in-process use inside an app: hooks the linker namespaces so system libvulkan loads the custom driver (hook_impl, needs a custom namespace/redirection; README does not document internals — mirror page). It works inside the app process (JNI), not obviously for a child exec'd from nativeLibDir (it patches in-process linker state) -> INFER: use (a) in the child instead. Licence BSD-2-Clause: fine as an external tool; do not copy into our MIT lib without attribution.
SELinux/KGSL access: Turnip opens /dev/kgsl-3d0 from the app process. Normal apps do open it (that is how the proprietary driver in the app process works), so the node is app-accessible (INFER/standard). New risks: (1) Android 16 QPR2 ioctl filtering could block KGSL ioctls not used by Qualcomm's blob (UNVERIFIED for Samsung/KGSL; Turnip uses more ioctls like IOCTL_KGSL_GPU_COMMAND, GPUOBJ_ALLOC with specific flags, perhaps perf counters). (2) Samsung Knox/Android 17 policy unknown. (3) Child process runs in the app's SELinux domain (untrusted_app), same as app (INFER).
Known field evidence: Turnip via adrenotools ran on non-rooted Snapdragon 8 Gen 3 / 8 Elite phones in emulators (Winlator etc.) (INFER from search results; not individually verified for Android 16/17 or Samsung one UI 8+).

Overall: feasible in principle in an unrooted app; success on S26 Ultra = UNKNOWN until we try (needs: gen8 KGSL support working, ioctl filter not blocking).

## 2. Building Turnip for arm64-android from source
- Source: Mesa (MIT; per-file SPDX/licence headers must be checked after checkout - not done here since checkout failed; Turnip files are MIT-style 'Copyright ... Google/Valve/Igalia', ir3 is MIT; some SPIR-V/NIR deps (spirv-tools Apache-2.0, vk-xml MIT-ish) are build-time deps - UNVERIFIED). Keep it a separate repo/artifact; not linked into our lib; ship only in the debug APK or as a user-pushed file to /data/local/tmp -> no licence mixing. If shipped inside an APK, include Mesa's MIT notice.
- Use a pinned Mesa release tag >= 26.0.x (or latest 26.x stable). Do NOT use unknown prebuilt ZIPs (Banners-Turnip, whitebelyash...) except as references to read patch sets.
- Plan (INFER, from the Mesa Android doc):
  1. CI job (ubuntu, NDK r27-r29 (note r29 needed small fixes per third-party page), meson, ninja, python mako/pyyaml, glslang? (Mesa vendors/needs glslang? only for some drivers), bison/flex, libdrm not needed with kgsl only).
  2. cross file android-aarch64 with api34 clang; options: -Dplatforms=android -Dandroid-stub=true -Dvulkan-drivers=freedreno -Dgallium-drivers= -Dfreedreno-kmds=kgsl -Dllvm=disabled -Dbuildtype=release -Dstrip=true -Dvulkan-layers= -Degl=disabled -Dglx=disabled. Wanted extra: also enable `msm` kmd? Not useful on Android.
  3. Output: src/freedreno/vulkan/libvulkan_freedreno.so (+ ICD json not needed for path (a)). Expected size ~10-20 MB stripped (INFER; third-party Turnip drivers are commonly 8-15 MB zipped; not verified).
  4. Patch our repro: `--icd <path>` that dlopens the ICD and uses vk_icdGetInstanceProcAddr (small shim in examples, not in libapplegfx core, or behind a build option).
  5. Package: libvulkan_freedreno.so in jniLibs/arm64-v8a in a DEBUG-only variant; log vkEnumeratePhysicalDevices result, device name, extension list.
- Effort (INFER): build in CI 1-2 days (dependency/NDK friction); ICD-direct shim in the repro tool 1-2 days; getting a first device result including debugging gen8 KGSL = 2-5+ days, open-ended if Turnip gen8 does not initialize on KGSL. Likely stop condition: vkCreateDevice fails / KGSL ioctl EPERM.
- Reference only: lavapipe is in the same Mesa tree (MIT) and builds natively on x86_64/arm64 Linux with -Dvulkan-drivers=swrast.

## 3. Validation layers and driver diagnostics
READ (Android NDK guide https://developer.android.com/ndk/guides/graphics/validation-layer):
- Layer VK_LAYER_KHRONOS_validation = libVkLayer_khronos_validation.so; put in jniLibs/arm64-v8a (Option A: bundled in APK). Prebuilt Android binaries from Khronos releases (Apache-2.0 per project; licence not shown on releases page, UNVERIFIED here). Latest release read: 1.4.363.0 (tag vulkan-sdk-1.4.363.0, "28 Sep", year not shown), built with NDK 27.3.13750724, requires Android API >= 26. https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases
- Enable per app (adb shell only, no root): settings put global enable_gpu_debug_layers 1; gpu_debug_app <pkg>; gpu_debug_layers VK_LAYER_KHRONOS_validation; gpu_debug_layer_app <pkg(s)>. Global (cleared on reboot, "affects all apps, including native executables"): adb shell setprop debug.vulkan.layers VK_LAYER_KHRONOS_validation. External layer loading needs debuggable app, userdebug, or targetSdk>=30 with meta-data com.android.graphics.injectLayers.enable=true.
- Messages: warnings/errors go to logcat tag VALIDATION by default; or through VK_EXT_debug_utils callback to our own sink.
- The Android loader doc says non-debuggable apps get layers only from native lib dir.
INFER / UNVERIFIED for OUR case (child process exec'd from nativeLibDir):
- The app-process layer path is set by GraphicsEnv (app namespace/lib path) in the app process. An exec'd native child does not go through that; it uses the default loader (libvulkan.so in system namespace). Whether the loader in the child finds libVkLayer_khronos_validation.so in nativeLibDir is UNVERIFIED and I'd bet it does NOT without help (INFER). `debug.vulkan.layers` is a loader-level property so it applies to any process but the layer .so must be dlopen-able by name from where the loader searches (/data/local/debug/vulkan only for debuggable processes in app context; for a plain exec'd process UNVERIFIED).
- Practical alternatives, in order of reliability:
  1. Make the app (or our repro as a JNI lib, i.e. the same code run inside the app process via JNI/dlopen instead of exec) the process that creates the Vulkan instance: then the standard layer discovery from nativeLibDir applies. Our repro code is a library function (lagfx_pipeline_repro_run) so this is cheap (hours-1 day).
  2. With the direct-ICD route (1a), skip the loader: also skip layers; instead link the validation layer .so ourselves: dlopen libVkLayer_khronos_validation.so and chain: its exported vkNegotiateLoaderLayerInterfaceVersion / vk_layerGetInstanceProcAddr need loader-provided chain info (VkLayerInstanceCreateInfo with pfnNextGetInstanceProcAddr). Doable manually (documented loader/layer interface) but 2-4 days and fiddly (INFER). Better: use the Khronos Vulkan-Loader built for Android as a static/standalone libvulkan in our nativeLibDir with explicit layer path via VK_LAYER_PATH / VK_ADD_LAYER_PATH (the Khronos loader honors env, Android system loader does not) (INFER; medium effort).
  3. Run validation on lavapipe/host with the same bundle (offline, section 5) - no device needed; catches spec violations in OUR SPIR-V/state. This is the most valuable cheap step.
- Header limit: our headers are 1.3.275 (per task description; I did not re-verify in third_party). Everything needed (debug_utils, pipeline_creation_feedback (core 1.3), pipeline_executable_properties (KHR, in 1.3.275)) is in 1.3.275. Newer validation layer (1.4.x) is independent of our headers (loader-layer interface is stable); it will warn for unknown newer features only if our app uses newer structs. Layer 1.4.363 on an Android 17 / Vulkan 1.4 loader is fine (INFER). Do not require apiVersion 1.4; keep 1.3.
Driver diagnostics (what can reach us):
- VK_EXT_debug_utils/debug_report: Qualcomm blob normally emits nothing useful through them for compiler failures (INFER; commonly only vkResult). Pipeline creation failure returns VK_ERROR_UNKNOWN / VK_ERROR_INITIALIZATION_FAILED / etc.; our repro already records the result code.
- VK_EXT_pipeline_creation_feedback: core in 1.3; Adreno 840 reports api 1.4.295 so it is available (READ api version; INFER the feedback works). It tells per-stage duration and whether cache hit - no compiler messages; on failure, no output (INFER). Cheap to add.
- VK_KHR_pipeline_executable_properties: UNVERIFIED for 840 (INFER: not exposed by Qualcomm blob; exposed by Turnip, per the "VK_pipeline_executable_properties loop count" commit in 26.0 (READ title) and lavapipe/others). Requires CAPTURE_STATISTICS/INTERNAL_REPRESENTATIONS flag at pipeline creation, so on Turnip we'd get ir3 disassembly even for the failing pipeline if Turnip accepts it.
- Qualcomm private diagnostics: no public env/props found (UNVERIFIED); do not guess.
- logcat: the app's own UID's log lines are readable by `logcat -d` run through Runtime.exec in the app (since JB, apps see only their own uid's logs; READ_LOGS not grantable to normal apps) (INFER from old CommonsWare/StackOverflow results in search; not re-verified for Android 17). Driver messages are logged under the process (pid/uid) of the app -> our child process's own logs (uid same) would be visible. The simpler route: the child's stdout/stderr pipes are read directly by the app (no logcat needed) + our LAGFX_LOG/ERR output; plus ApplicationExitInfo (API 30+) to get native crash reason/tombstone trace for the app's own process (INFER; not verified). Tombstones/dumpsys/logcat -b all for other processes need adb.
- What an unattended app can do: spawn the child, capture stdout/stderr + exit status/signal, save to app files dir, optionally `logcat -d -b main,system,crash` filtered to own uid. No system diagnostics (dumpsys gfxinfo? other uids) without adb.

### Exact adb commands for the user (no root needed unless stated)
(READ = in NDK guide; the rest is standard adb behavior, INFER)
```
# full log, capture while reproducing
adb logcat -b all -c
adb logcat -b all -v threadtime > henge-vk.log      # Ctrl-C after repro
adb logcat -d -b all -v threadtime > henge-vk.log   # or dump after the fact

# validation layer (needs a debuggable app OR layer in app + injectLayers meta-data; settings need no root)
adb shell settings put global enable_gpu_debug_layers 1
adb shell settings put global gpu_debug_app <pkg>
adb shell settings put global gpu_debug_layers VK_LAYER_KHRONOS_validation
adb shell settings put global gpu_debug_layer_app <pkg>
adb logcat -s VALIDATION
# cleanup
adb shell settings delete global enable_gpu_debug_layers; adb shell settings delete global gpu_debug_app; adb shell settings delete global gpu_debug_layers; adb shell settings delete global gpu_debug_layer_app

# global, affects native executables too, cleared on reboot (setprop of debug.* from adb shell normally works without root: INFER/UNVERIFIED on Samsung)
adb shell setprop debug.vulkan.layers VK_LAYER_KHRONOS_validation
adb shell getprop | grep -i vulkan

# device/driver info
adb shell dumpsys SurfaceFlinger | grep -iE "gpu|vulkan"
adb shell getprop ro.hardware.vulkan; adb shell getprop ro.vulkan.apex; adb shell getprop ro.board.platform
adb shell cmd gpu vkjson            # may not exist/permitted (UNVERIFIED); gives extension list JSON
adb shell dumpsys gpu               # GPU service info incl. driver versions (UNVERIFIED permissions)
# run repro child by hand as the app uid (needs a debuggable build for run-as):
adb shell run-as <pkg> sh -c 'cd files && LAGFX_VK_VALIDATION=1 <nativeLibDir>/libg7pipelinerepro.so <bundle dir> --matrix'
```
Needs root (do not use): remount /vendor driver replacement, setenforce, reading /sys/class/kgsl internals, FD_RD_DUMP to system paths (also MSM-only).

## 4. Cheap wins in our own tool (INFER; small)
- Dump vkEnumerateDeviceExtensionProperties + VkPhysicalDeviceProperties (driverVersion, driverID/driverName via VkPhysicalDeviceVulkan12Properties) in the repro report -> answers the pipeline_executable_properties question and records Qualcomm driver version for upstream reports.
- Chain VkPipelineCreationFeedbackCreateInfo and, if available, set CAPTURE flags and read executable statistics on success variants in --matrix.
- Route debug_utils messages to stdout of the repro (not only the env-gated path).

## 5. What we can do offline now (no device)
1. Run the failing bundle (pipefail_*/{vs,fs}.spv + state) through the repro tool on lavapipe (x86_64 Linux, Mesa swrast Vulkan) with LAGFX_VK_VALIDATION=1: tells whether the SPIR-V + state is legal; if lavapipe builds it and the device fails, the blame is more likely driver compiler (INFER; lavapipe is far more permissive, absence of failure proves little).
2. spirv-val (Khronos SPIRV-Tools, Apache-2.0) with --target-env vulkan1.3 on each .spv; spirv-opt/spirv-reduce loop with the device as oracle: the oracle is "run repro on device, exit code 0/1" - requires the loop to push candidate SPIR-V to the phone via adb (adb push + run-as + pull result); spirv-reduce interestingness script can do this. ~1 day, slow per iteration (seconds). Needs a bundle format where only the .spv is swapped (the existing bundle layout supports it: fs.spv/vs.spv + state).
3. Turnip on x86_64 host WITHOUT a GPU: UNVERIFIED. Turnip needs a kernel backend (msm DRM, kgsl, virtio-gpu native context); I did not find a software/null path I could confirm. I recall Mesa having a null/offline mode for compiling with freedreno's ir3 (e.g., ir3 offline compiler for a given GPU id), which can compile shaders but cannot create a pipeline/render. So: cannot render without the GPU; shader-compile-only might be possible (UNVERIFIED), and it would exercise Mesa's compiler, not Qualcomm's, so it does not reproduce the Qualcomm failure.
4. Run Mesa's validation of gen8 spec-conformance: not possible offline.

## Verdicts
| Item | Verdict | Effort | Main risk |
| lavapipe + validation on same bundle (host) | feasible now | hours | lavapipe permissive |
| spirv-val / reduce with device oracle | feasible now | ~1 day | slow adb loop |
| Validation layer on device from repro child | needs work | 1-4 days (best: run repro in app process, or Khronos loader with explicit layer path) | layer discovery in exec'd child unverified |
| Extension list / creation_feedback / driver version dump | feasible now | hours | none |
| pipeline_executable_properties on 840 | unknown (probably absent) | check by dump | - |
| Turnip build for arm64-android from Mesa source | feasible (CI) | 1-2 days | NDK/meson friction |
| Turnip on unrooted S26 Ultra (840, KGSL) | unproven, may be not feasible | 2-5+ days, open-ended | gen8 KGSL support, Android 16/17 ioctl filtering, Samsung SELinux |
| Qualcomm compiler messages to app | essentially not available | - | blob prints nothing via public API (INFER) |

## Could NOT verify
- Mesa source (clone blocked, raw gitlab robots-blocked): exact gen8 KGSL status, extension gating on gen8, per-file licence headers, build size.
- Any run of Turnip/validation on the device.
- Whether Adreno 840 exposes VK_KHR_pipeline_executable_properties.
- Whether the Android loader finds layers for an exec'd child in nativeLibDir.
- Whether KGSL ioctls Turnip needs are blocked by Android 16 QPR2/17 filtering or Samsung policy.
- Validation-layer release licence/asset names (releases page lacked filenames), release year.
- Exact header version in our tree (1.3.275 taken from the task statement).

## Sources
https://www.phoronix.com/news/Mesa-26.0-Adreno-Gen-8-Graphics ; https://docs.mesa3d.org/relnotes/26.0.0 ; https://docs.mesa3d.org/android.html ; https://docs.mesa3d.org/drivers/freedreno.html ; https://fuchsia.googlesource.com/third_party/mesa/+/main/docs/relnotes/24.3.0.rst ; https://www.androidauthority.com/snapdragon-8-elite-8-elite-gen-5-devices-emulation-driver-3617718 ; https://newreleases.io/project/github/The412Banner/Banners-Turnip/release/v26.3.0-20260916 ; https://gitee.com/yuzu-mirror/libadrenotools (BSD-2-Clause) ; https://source.android.com/docs/core/graphics/implement-vulkan ; https://source.android.com/docs/security/features/gpu-syscall-filtering ; https://blog.google/security/further-hardening-android-gpus/ ; https://developer.android.com/ndk/guides/graphics/validation-layer ; https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases

## 4. Galaxy Tab S8 (Adreno 730, stock driver Vulkan 1.1.128) — can Turnip replace the legacy-render-pass work?  (added 2026-11-28; research only, nothing downloaded or run)
Legend as above. This section answers a reviewer note (Turnip could give the S8 Dynamic Rendering / Local Read and make the Legacy path unnecessary). It is kept apart from the main S8 path (Legacy render pass on the stock driver continues).
READ (web search this session):
- Banners-Turnip (third-party, automated builds, "not guaranteed stable") publishes an "A6xx / A7xx — Standard" ZIP described as targeting Snapdragon 600–800 series, 7 Gen and 8 Gen 1–3 builds; newest build I could see: v26.3.0-20260911-r7, Vulkan 1.4.362 (Mesa 26.3.0 line). Adreno 730 (8 Gen 1) is in the stated target range.
- The reviewer's figures "Mesa 27.0.0-devel / Vulkan 1.4.368 / 2026-11-26" were not found by my first searches, but a later fetch of the Banners-Turnip release page v27.0.0-20261127-r5 CONFIRMS them: Mesa main commit 1988f68, Vulkan 1.4.368, commit date 2026-11-27, Standard build range "Adreno 6xx / 7xx (8 Gen 3 and older)" (no numbered Mesa release given on that page). Adreno 730 is inside the stated range; running it on the S8 is still UNVERIFIED.
- libadrenotools (BSD-2-Clause): rootless loading of a custom Adreno Vulkan driver, Android 9+, arm64; the stock driver stays installed and other apps are unaffected (several emulators use it with Turnip). The loader is meant to be used inside the app process.
INFER / UNVERIFIED (must be answered on the S8 itself):
- Which of dynamic_rendering, dynamic_rendering_local_read, rasterization_order_attachment_access, multisampled local read, int64/atomic64, demote, BDA a given Turnip build exposes ON Adreno 730 (Mesa has had the extensions since 24.3 / earlier "where implemented"; per-generation enablement is not verified). Reported Vulkan version is a driver property, not a statement about these features.
- Whether Turnip's KGSL backend works on this S8's firmware / One UI, and whether the app-process Samsung GPU ioctl policy allows it.
Constraints from the repo's own rules: no unknown prebuilt ZIPs in the product path (section 2: build Turnip from a pinned Mesa tag in CI, or use a third-party ZIP only as a clearly separated research probe); the existing probe launcher runs as an exec'd child, where section 1 option (a) (dlopen the ICD and use vk_icdGetInstanceProcAddr) fits better than in-process libadrenotools hooks.
Proposed probe order (not started): (1) read-only probe printing apiVersion, extension list, feature structs and properties for stock vs Turnip on the S8 (reuse the g7-probe output format); (2) only if (1) shows dynamic_rendering: Device creation, one clear/draw/MSAA-resolve pixel test through the existing Dynamic path; (3) only then a driver-selection option in Henge. Needs the user's decision on running any third-party driver binary on their device.
