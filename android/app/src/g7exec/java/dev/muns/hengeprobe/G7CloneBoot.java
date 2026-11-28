package dev.muns.hengeprobe;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import java.util.function.IntSupplier;

/**
 * [G7-EXEC] Only in the g7exec source set, and the ONLY place that starts the clone-boot runner (libg7cloneboot.so = host/ui_runner_vm_clone.c): a normal (non-installer) boot of a VERIFIED CLONE of the
 * install pair, whose disk and AUX the runner may write. It is a boot test of the COPY: it never names the original pair's files.
 *
 * Before a process is created, in this order, each refusing on its own: the build's start switch (the same switch as the restore and the deletion: false in the g7exec build, true only in the start build);
 * a typed consent; no live G7 run and no live clone boot; no runner process found (or unknown); no unfinished isolation; the runner file packaged; a usable ROM; a valid ECID; and G7Clone.validate() of the
 * folder (finished marker, only known files, no links, sizes, ECID tag equal to the current ECID). The ECID is not on the command line: the runner reads vm_ecid.txt from --ecid-dir itself.
 * The run is bounded (RUN_SECONDS), writes its own log (g7_cboot_*.txt, CREATE_NEW) and records the stage estimate (G7BootStage). Cancel asks the runner to stop ("quit"), then ends it after a wait.
 * No Android classes: tests/java/G7CloneBootTest.java runs on a plain JVM with a fake process.
 */
public final class G7CloneBoot {
    private G7CloneBoot() {}

    static final String LIB_NAME = "libg7cloneboot.so";
    static final int RUN_SECONDS = 900;
    /** The run times a person may choose (seconds; 0 = no limit, the run then ends by the stop button, a guest end, a fatal engine error or the app closing). Anything else is refused. The stop, quiesce and flush path is the same for all of them. */
    public static final int[] RUN_SECONDS_CHOICES = {180, 900, 1800, 3600, 0};
    static boolean validSeconds(int seconds) { for (int c : RUN_SECONDS_CHOICES) if (c == seconds) return true; return false; }
    /** JIT code region sizes (MiB) a person may choose for the runner's --jit-mb (the runner itself accepts 16..768). The default 64 is what every earlier build passed; anything outside this set is refused here and never reaches the command line. */
    public static final int[] JIT_MB_CHOICES = {64, 128, 256, 384, 512, 768};
    public static final int JIT_MB_DEFAULT = 256;
    static boolean validJitMb(int mb) { for (int c : JIT_MB_CHOICES) if (c == mb) return true; return false; }
    /** Chosen on the start dialog; read once per start. */
    static volatile int jitMb = JIT_MB_DEFAULT;
    /** Test hook (no Intent input exists in this build): a one-line marker file "jit_mb" next to vm_ecid.txt, e.g. adb shell run-as <pkg> sh -c 'echo 256 > files/vm/jit_mb' (same folder as nvram_preseed). It only PRESELECTS the dialog's spinner; the person still starts the run. Missing, unreadable, larger than 16 bytes, a link, or a value outside JIT_MB_CHOICES = JIT_MB_DEFAULT (256). */
    public static int readJitMarker(File priv) {
        try {
            java.nio.file.Path p = new File(priv, "jit_mb").toPath();
            if (!Files.isRegularFile(p, java.nio.file.LinkOption.NOFOLLOW_LINKS) || Files.size(p) > 16) return JIT_MB_DEFAULT;
            String t = new String(Files.readAllBytes(p), StandardCharsets.US_ASCII).trim();
            if (!t.matches("[0-9]{1,4}")) return JIT_MB_DEFAULT;
            int v = Integer.parseInt(t);
            return validJitMb(v) ? v : JIT_MB_DEFAULT;
        } catch (IOException | RuntimeException e) { return JIT_MB_DEFAULT; }
    }
    /** Exact "translated before" set of the engine's stats (core/jitstats.h; env G7_STATS_EXACT = max keys in thousands). Default OFF, never remembered. It costs memory (up to a 96 MiB table, 144 MiB while it grows) and a little time under the translation lock, so it is OFF for general A/B runs; the plan is jit-mb 64 (J0) against 256 (J1) with it OFF, J2 = 128, J3 = 512, J4 = exact ON. Only the one validated value STATS_EXACT_VALUE is ever passed (no other number, no HV_ name: the runner scrubs HV_*). */
    public static final String STATS_EXACT_VALUE = "2048";
    static volatile boolean statsExact = false;
    /** Test hook, same shape as jit_mb: a one-line marker file "jit_exact" next to vm_ecid.txt whose content is exactly 1 PRESELECTS the dialog's checkbox (adb shell run-as <pkg> sh -c 'echo 1 > files/vm/jit_exact'). Missing, unreadable, larger than 16 bytes, a link, or any other content = OFF. The person still starts the run. */
    public static boolean readStatsExactMarker(File priv) {
        try {
            java.nio.file.Path p = new File(priv, "jit_exact").toPath();
            if (!Files.isRegularFile(p, java.nio.file.LinkOption.NOFOLLOW_LINKS) || Files.size(p) > 16) return false;
            return "1".equals(new String(Files.readAllBytes(p), StandardCharsets.US_ASCII).trim());
        } catch (IOException | RuntimeException e) { return false; }
    }
    /** integ7b (T2): the peak-memory budget (MiB) of the exact set while its table grows (G7_STATS_EXACT_PEAK_MIB, core/jitstats.h: a growth whose old + new table would pass it is refused). The table is up to 96 MiB (2^22 slots, 2,936,012 keys) and up to 144 MiB at its peak while the last
     *  doubling runs (48 + 96). With 100 the last doubling is refused: the table stays at 48 MiB (peak 72 MiB, about 1.47 M keys), so it CANNOT record all of the ~2 M unique blocks a boot translates; the rest are counted as exact_overflow and the
     *  retranslation figures are then PARTIAL. 150 and 200 allow the full table (peak 144 MiB). 0 = unlimited = the variable is not set. Default 100: the smaller footprint on a phone; choose 150 for a complete set. Only these four values can be passed. */
    public static final int[] STATS_EXACT_PEAK_CHOICES = {100, 150, 200, 0};
    public static final int STATS_EXACT_PEAK_DEFAULT = 100;
    static boolean validStatsExactPeak(int v) { for (int c : STATS_EXACT_PEAK_CHOICES) if (c == v) return true; return false; }
    static volatile int statsExactPeakMib = STATS_EXACT_PEAK_DEFAULT;
    static String statsExactPeakName(int v) { return v == 0 ? "unlimited" : String.valueOf(v); }
    /** Test hook like jit_exact: a one-line marker file "jit_exact_peak" next to vm_ecid.txt, content exactly 100, 150, 200 or unlimited, PRESELECTS the spinner. Anything else (missing, link, over 16 bytes, other content) = the default 100. */
    public static int readStatsExactPeakMarker(File priv) {
        try {
            java.nio.file.Path p = new File(priv, "jit_exact_peak").toPath();
            if (!Files.isRegularFile(p, java.nio.file.LinkOption.NOFOLLOW_LINKS) || Files.size(p) > 16) return STATS_EXACT_PEAK_DEFAULT;
            String t = new String(Files.readAllBytes(p), StandardCharsets.US_ASCII).trim();
            for (int c : STATS_EXACT_PEAK_CHOICES) if (statsExactPeakName(c).equals(t)) return c;
            return STATS_EXACT_PEAK_DEFAULT;
        } catch (IOException | RuntimeException e) { return STATS_EXACT_PEAK_DEFAULT; }
    }
    /** What the exact set costs and can do, in words, for the dialog, the log header and the notes. */
    static String statsExactCapacityText(int peak) {
        return peak == 100 ? "peak budget 100 MiB: the table stops at 48 MiB (peak 72 MiB, about 1.47 M keys), so it CANNOT record all of a boot's ~2 M unique blocks; the rest are counted as exact_overflow and the exact_* figures are PARTIAL"
                : "peak budget " + statsExactPeakName(peak) + ": table up to 96 MiB final and up to 144 MiB peak while it grows (48 + 96), 2,936,012 keys, enough for ~2 M unique blocks";
    }
    static String statsExactText(boolean on) { return on ? "ON (G7_STATS_EXACT=" + STATS_EXACT_VALUE + ", " + statsExactCapacityText(statsExactPeakMib) + ")" : "OFF"; }
    /** One line at the top of the run log: which engine diagnostics are on in this run (the engine's own --stats lines every 10 s are always on in the clone runner). */
    static String engineDiagLine(boolean exact) {
        return "g7-engine-diag: stats_lines=ON (every 10 s, always); exact_seen_set=" + statsExactText(exact) + "; draw_diag=" + (drawDiag ? "ON" : "OFF") + "; fb0=" + (fb0 ? "ON" : "OFF") + "; display_events=" + (displayEvents ? "ON" : "OFF")
                + "; vbl_exp=" + (validVblExp(vblExp) ? vblExp : "?") + "; gpu_exec=" + (gpuExec ? "ON" : "OFF") + "; gpu_dump=" + (gpuDumpDir != null ? "ON" : "OFF") + "; render_replay=" + (captureData ? "REQUESTED" : "OFF") + "\n";
    }
    static final String LOG_PREFIX = "g7_cboot_", LOG_SUFFIX = ".txt";
    static final int MAX_LOG_BYTES = 2 * 1024 * 1024;
    static final long STATUS_EVERY_MS = 30_000, STOP_WAIT_MS = 15_000;

    interface Spawner { Process spawn(List<String> command) throws IOException; }
    private static final Spawner REAL = c -> {
        ProcessBuilder pb = new ProcessBuilder(c).redirectErrorStream(true);
        lagfxEnv(pb.environment(), c);
        return pb.start();
    };

    static final String LAGFX_LOG_PREFIX = "g7_lagfx_";
    /** Two LAGFX_ variables for the lagfx build of the runner (libapplegfx-vulkan reads them; the normal runner ignores them, and the runner removes only HV_* ones): the log goes to its own file next to the run
     *  log, and at trace level (the guest's GPU commands). Not an HV_ name, so the runner's scrub does not touch it. The folder comes from the --shm path, which is always in the app's files folder. */
    /** Experimental GPU switches chosen on the start dialog (read when the runner is started; default off). gpuExec = LAGFX_ARM64_EXEC=1 (really execute the guest's Render/Blit commands on Vulkan); gpuDumpDir != null =
     *  LAGFX_ARM64_DUMP=1 plus LAGFX_ARM64_DUMP_DIR (the library saves the guest's pipeline/shader data there, size-capped, new files only). */
    /** Name of the VM log (g7_cboot_*) of the run being started; written to run-info.txt so the export needs no time matching. */
    static volatile String vmLogName = "";
    static volatile boolean gpuExec = false;
    /** Experiment switch, independent of gpuExec (only meaningful with it): LAGFX_ARM64_MAPPED_WRITEBACK. 0 = not set, 1 = "1" (write back after every exec that drew), 2 = "swap" (owed surfaces are paid before a display command). */
    static volatile int gpuWritebackMode = 0;
    /** integ7b T7: independent readback (LAGFX_GPU_READBACK=1) and write-back verify (LAGFX_WB_VERIFY=1) of the library. Default OFF, never remembered, only set when exactly asked (otherwise the variable is removed, never "0"); logged in g7-gpu-options. WB_VERIFY alone does not enable the readback sync. */
    static volatile boolean gpuReadback = false, wbVerify = false, gammaApply = false, msLocalReadForce = false;   // gammaApply: LAGFX_GAMMA_APPLY=1; msLocalReadForce: LAGFX_VK_FORCE_MS_LOCAL_READ=1 (device property says no, run it anyway). Both default OFF, never remembered.
    static volatile boolean captureData = false;   // LAGFX_ARM64_DUMP_DATA=1 (Render Replay capture: buffers/textures/attachments of up to 256 draws, 128 MiB total; needs the dump + GPU execution)
    /** Per-draw diagnostics of the library (G7_DRAW_DIAG; not an HV_ name, so the runner's scrub leaves it alone). Default ON: the library's own default is on, the dialog always sends an explicit 1 or 0. Independent of captureData (Render Replay). */
    static volatile boolean drawDiag = true;
    /** Early framebuffer observation (G7_FB0=1; host/g7_fb0.h: "g7-fb0:" lines, 250 ms x 60 s, at most 3 dumps). integ7b: default OFF (G7_FB0 is not set at all); the dialog checkbox, never remembered, turns it on for the dedicated F0 run. The runner logs the real cost (total_ms/max_ms) in its end line. */
    static volatile boolean fb0 = false;
    /** VBL experiment switch (research/g7-hjit2; env LAGFX_VBL_EXP read by devices/pvg_lite.c; only meaningful together with the display-events checkbox). "0" = off = the behaviour of every earlier build, and then the variable is NOT set at all. */
    public static final String[] VBL_EXP_CHOICES = {"0", "tick_none", "bit3_once", "swap_no_or2", "bit3_on_enable"};
    static boolean validVblExp(String v) { for (String c : VBL_EXP_CHOICES) if (c.equals(v)) return true; return false; }
    static volatile String vblExp = "0";
    static volatile boolean displayEvents = false;   // HV_PVG_DISPLAY_EVENTS: VBL + display cause bit (experiment)
    static volatile File gpuDumpDir = null;

    static void lagfxEnv(java.util.Map<String, String> env, List<String> cmd) {
        int i = cmd.indexOf("--shm");
        if (i < 0 || i + 1 >= cmd.size()) return;
        File dir = new File(cmd.get(i + 1)).getParentFile();
        if (dir == null) return;
        env.put("LAGFX_LOG_LEVEL", "trace");
        final File lagfxLog = new File(dir, LAGFX_LOG_PREFIX + String.format("%013d", System.currentTimeMillis()) + ".txt");
        env.put("LAGFX_LOG_FILE", lagfxLog.getPath());
        StringBuilder note = new StringBuilder();
        int ji = cmd.indexOf("--jit-mb"); String jitArg = ji >= 0 && ji + 1 < cmd.size() ? cmd.get(ji + 1) : "?";
        note.append("gpu_exec=" + (gpuExec ? "ON" : "OFF") + " (start dialog checkbox: GPU 실제 실행); ");   // first item on purpose: the state of the one switch that changes what the library does
        note.append("jit_mb=" + jitArg + "; ");
        final boolean exact = statsExact;   // read once for the note, the environment and run-info.txt
        note.append("jit_exact=" + statsExactText(exact) + "; ");
        note.append("build henge=" + G7ExecLauncher.HENGE_SHA + " lagfx=" + G7ExecLauncher.LAGFX_SHA + "; ");   // ties every log to the exact commits (integration-APK traceability)
        env.remove("LAGFX_ARM64_EXEC"); env.remove("LAGFX_ARM64_MAPPED_WRITEBACK"); env.remove("HV_PVG_DISPLAY_EVENTS"); env.remove("G7_PVG_DISPLAY_EVENTS"); env.remove("LAGFX_ARM64_DUMP_DATA"); env.remove("G7_DRAW_DIAG"); env.remove("G7_FB0"); env.remove("LAGFX_VBL_EXP"); env.remove("LAGFX_PIPELINE_MATRIX"); env.remove("LAGFX_WB_VERIFY"); env.remove("LAGFX_GPU_READBACK"); env.remove("G7_STATS_EXACT"); env.remove("G7_STATS_EXACT_PEAK_MIB"); env.remove("HV_STATS_EXACT"); env.remove("HV_STATS_EXACT_PEAK_MIB");   // never inherit these from the parent environment: the start dialog alone decides
        if (exact) {   // the only place it is set; absent (not "0") when OFF
            env.put("G7_STATS_EXACT", STATS_EXACT_VALUE);
            int pk = statsExactPeakMib;   // one validated value; unlimited (0) = the variable stays unset
            if (validStatsExactPeak(pk) && pk > 0) env.put("G7_STATS_EXACT_PEAK_MIB", String.valueOf(pk));
        }
        if (drawDiag) { env.put("LAGFX_PIPELINE_MATRIX", "1"); note.append("pipeline_matrix=ON (LAGFX_PIPELINE_MATRIX=1, with draw diag; runs on pipeline creation failure only); "); } env.put("G7_DRAW_DIAG", drawDiag ? "1" : "0"); note.append("draw_diag=" + (drawDiag ? "ON" : "OFF") + " (G7_DRAW_DIAG=" + (drawDiag ? "1" : "0") + "); ");
        { String vx = vblExp; if (validVblExp(vx) && !"0".equals(vx)) { env.put("LAGFX_VBL_EXP", vx); note.append("vbl_exp=" + vx + " (LAGFX_VBL_EXP, device experiment; needs display events ON to matter except swap_no_or2); "); } else note.append("vbl_exp=off; "); }
        if (gpuReadback) env.put("LAGFX_GPU_READBACK", "1");
        if (wbVerify) env.put("LAGFX_WB_VERIFY", "1");
        env.remove("LAGFX_GAMMA_APPLY"); env.remove("LAGFX_VK_FORCE_MS_LOCAL_READ");   // a value inherited from the launching environment must not switch an experiment on
        if (gammaApply) env.put("LAGFX_GAMMA_APPLY", "1");
        if (msLocalReadForce) env.put("LAGFX_VK_FORCE_MS_LOCAL_READ", "1");
        note.append("gamma_apply=" + (gammaApply ? "ON (LAGFX_GAMMA_APPLY=1; plane order R,G,B and colour space ASSUMED)" : "OFF (unset)") + "; ms_local_read_force=" + (msLocalReadForce ? "ON (LAGFX_VK_FORCE_MS_LOCAL_READ=1; outside the Vulkan spec when the device reports 0)" : "OFF (unset)") + "; ");
        note.append("gpu_readback=" + (gpuReadback ? "ON (LAGFX_GPU_READBACK=1)" : "OFF (unset)") + "; " + WBV_MARK + "; ");   // the effective value is filled in at the end from the environment actually built (the write-back mode also sets it)
        if (fb0) env.put("G7_FB0", "1"); note.append("fb0=" + (fb0 ? "ON (G7_FB0=1" : "OFF (G7_FB0 unset") + ", early framebuffer observation, g7-fb0: lines; default OFF: enable only in the dedicated F0 run, keep OFF in J0/J1 timing comparisons); ");
        if (displayEvents) { env.put("G7_PVG_DISPLAY_EVENTS", "1"); note.append("display events experiment ON (G7_PVG_DISPLAY_EVENTS=1, read by the runner after its HV_* scrub); "); } if (gpuExec) { env.put("LAGFX_ARM64_EXEC", "1"); note.append("GPU execution ON (LAGFX_ARM64_EXEC=1); "); if (gpuWritebackMode == 1) { env.put("LAGFX_ARM64_MAPPED_WRITEBACK", "1"); note.append("write-back experiment ON, immediate (LAGFX_ARM64_MAPPED_WRITEBACK=1: a rendered display surface is written to the guest after each exec); "); } else if (gpuWritebackMode == 2) { env.put("LAGFX_ARM64_MAPPED_WRITEBACK", "swap"); note.append("write-back experiment ON, deferred (LAGFX_ARM64_MAPPED_WRITEBACK=swap: owed surfaces are written before each display command); "); } else note.append("write-back experiment off; "); if (gpuWritebackMode != 0) { env.put("LAGFX_WB_VERIFY", "1"); note.append("write-back verify ON (LAGFX_WB_VERIFY=1, read-only, max 8 compares, 10 s apart); "); } } else note.append("GPU execution off; ");
        File dd = gpuDumpDir;
        if (dd == null) note.append("GPU data dump off");
        else {
            File runDir = new File(dd, "run-" + new java.text.SimpleDateFormat("yyyyMMdd-HHmmss-SSS", java.util.Locale.US).format(new java.util.Date()));   // one folder per run: no mixing, no name clashes
            try {
                java.nio.file.Files.createDirectories(runDir.getParentFile().toPath());
                java.nio.file.Files.createDirectory(runDir.toPath());   // must be NEW: an existing folder is an error, never reused
                env.put("LAGFX_ARM64_DUMP", "1");
                env.put("LAGFX_ARM64_DUMP_DIR", runDir.getPath());
                if (captureData && gpuExec) { env.put("LAGFX_ARM64_DUMP_DATA", "1"); note.append("render_replay=ON (LAGFX_ARM64_DUMP_DATA=1, library caps: 256 draws / 128 MiB); "); }
                try {   // ties this dump to its lagfx log and to the build (the export reads it; older dumps have none and are matched by time)
                    String info = "run_id=" + runDir.getName().substring(4) + "\nlagfx_log=" + lagfxLog.getName() + "\nvm_log=" + vmLogName + "\ngpu_exec=" + gpuExec + "\njit_mb=" + jitArg + "\njit_exact=" + (exact ? "ON" : "OFF") + "\njit_exact_peak_mib=" + (exact ? statsExactPeakName(statsExactPeakMib) : "n/a") + "\nhenge_commit=" + G7ExecLauncher.HENGE_SHA + "\nlagfx_commit=" + G7ExecLauncher.LAGFX_SHA + "\n";
                    java.nio.file.Files.write(new File(runDir, G7ExecDumpExport.INFO).toPath(), info.getBytes(java.nio.charset.StandardCharsets.UTF_8), java.nio.file.StandardOpenOption.CREATE_NEW);
                } catch (java.io.IOException | RuntimeException e) { note.append("(run-info.txt not written: ").append(e.getClass().getSimpleName()).append(") "); }
                note.append("GPU data dump ON, folder ").append(runDir.getPath()).append(" (limits: the library's own defaults, set by LAGFX_ARM64_DUMP_MAX_FILE / _MAX_TOTAL)");
            } catch (java.io.IOException | RuntimeException e) {
                note.append("GPU data dump REQUESTED BUT NOT POSSIBLE: cannot create ").append(runDir.getPath()).append(" (").append(e.getClass().getSimpleName()).append(")");
            }
        }
        if (!env.containsKey("LAGFX_ARM64_DUMP_DATA")) note.append(captureData ? "; render_replay=REQUESTED BUT NOT ACTIVE (needs gpu_exec=ON and the shader/pipeline dump folder)" : "; render_replay=OFF");
        lastGpuNote = note.toString().replace(WBV_MARK, wbVerifyText(env.get("LAGFX_WB_VERIFY"), wbVerify, gpuExec ? gpuWritebackMode : 0));
    }

    private static final String WBV_MARK = "@WBV@";
    /** wb_verify for the g7-gpu-options note: what the checkbox requested, what the environment actually got (LAGFX_WB_VERIFY) and why. Text only. */
    static String wbVerifyText(String envValue, boolean checkbox, int wbMode) {
        boolean eff = "1".equals(envValue), wb = wbMode == 1 || wbMode == 2;
        String reason = !eff ? "none" : checkbox && wb ? "checkbox+" + (wbMode == 1 ? "immediate" : "deferred") + "_writeback" : checkbox ? "checkbox" : wb ? (wbMode == 1 ? "immediate" : "deferred") + "_writeback" : "unknown";
        return "wb_verify_requested=" + (checkbox ? "ON(checkbox)" : "OFF") + "; wb_verify_effective=" + (eff ? "ON (LAGFX_WB_VERIFY=1)" : "OFF (unset)") + "; wb_verify_reason=" + reason;
    }

    /** What lagfxEnv decided about the GPU switches, written at the top of the run log so a missing dump can be explained. */
    static volatile String lastGpuNote = "";
    static volatile String lastRunConfig = "";

    /** The newest lagfx log on disk (g7_lagfx_*.txt), or null. */
    public static File newestLagfxLog(File filesDir) {
        File[] l = filesDir.listFiles((d, n) -> n.matches("g7_lagfx_[0-9]{13}\\.txt"));
        if (l == null || l.length == 0) return null;
        Arrays.sort(l);
        return l[l.length - 1];
    }

    public static final class Start {
        public final boolean started; public final String message; public final Run run;
        Start(boolean s, String m, Run r) { started = s; message = m; run = r; }
    }

    static volatile Run CURRENT;
    static boolean hasLive() { Run c = CURRENT; return c != null && !c.finished(); }

    /** The clone folders under vm/clones/ that pass G7Clone.validate() for this ECID (the list the person picks from). Looks only. */
    public static List<File> listValid(File privIn) {
        List<File> out = new ArrayList<>();
        File priv; try { priv = privIn.getCanonicalFile(); } catch (IOException e) { return out; }   // the app's folder is reached through a link on Android: validate() wants plain paths
        long ecid = G7ProbeLauncher.readEcid(priv);
        File[] k = new File(priv, "clones").listFiles();
        if (k == null || ecid <= 0) return out;
        Arrays.sort(k);
        for (File d : k) if (G7Clone.validate(d, priv, ecid) == null) out.add(d);
        return out;
    }

    /** One line per folder under vm/clones/: usable, or why it cannot be booted (so a clone that cannot boot is shown as such, not hidden). */
    public static List<String> describeAll(File privIn) {
        List<String> out = new ArrayList<>();
        File priv; try { priv = privIn.getCanonicalFile(); } catch (IOException e) { out.add("앱 폴더 경로를 확인하지 못했습니다"); return out; }
        long ecid = G7ProbeLauncher.readEcid(priv);
        File[] k = new File(priv, "clones").listFiles();
        if (k == null) return out;
        Arrays.sort(k);
        for (File d : k) { String bad = G7Clone.validate(d, priv, ecid); out.add(d.getName() + ": " + (bad == null ? "부팅 가능(검증됨)" : "부팅 불가 - " + bad)); }
        return out;
    }

    public static Run current() { return CURRENT; }

    /** The newest clone-boot log on disk (g7_cboot_*.txt), or null. Independent of any Run object, so it still works after the app was restarted. */
    public static File newestCloneLog(File filesDir) {
        File[] l = filesDir.listFiles((d, n) -> n.matches("g7_cboot_[0-9]{13}\\.txt"));
        if (l == null || l.length == 0) return null;
        Arrays.sort(l);
        return l[l.length - 1];
    }

    /** The saved clone-boot log text (up to the log limit), "" if none. */
    public static String readSavedCloneLog(File filesDir) {
        File f = newestCloneLog(filesDir);
        return f == null ? "" : G7ExecLauncher.readTail(f, MAX_LOG_BYTES + 8192);
    }

    /** Status text from the newest saved clone-boot log (no Run needed): ended with an exit line, or no exit record (process ended unseen). */
    public static String describeSaved(File filesDir) {
        File f = newestCloneLog(filesDir);
        if (f == null) return "";
        String t = readSavedCloneLog(filesDir);
        int exit = Integer.MIN_VALUE;
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("(?m)^g7-cboot-exit: (-?\\d+)").matcher(t);
        if (m.find()) exit = Integer.parseInt(m.group(1));
        String verdict = null;
        m = java.util.regex.Pattern.compile("(?m)^g7-cboot-verdict: (.*)$").matcher(t);
        if (m.find()) verdict = m.group(1);
        return "[저장된 복제본 부팅 로그: " + f.getName() + "]\n" + G7CloneMonitor.replay(t).render(0, false, exit, "저장 로그의 g7-stage 줄 참고", verdict, "저장 로그에는 화면 이미지가 없습니다");
    }

    public static boolean isPackaged(File nativeLibDir) { return nativeLibDir != null && new File(nativeLibDir, LIB_NAME).isFile(); }

    /** LOW-RESOLUTION DIAGNOSTIC (T9): the hex FRAME line, at most 96x64 RGB. It feeds the class/statistics text only; the picture the screen shows comes from the full-resolution file (G7FrameFile). One picture the runner printed (a downsampled copy of what the machine shows, at most 96x64, RGB) and its pixel-statistics class. The class is an ESTIMATE, not a decision about what is on the screen. */
    public static final class Frame {
        public final int w, h; public final byte[] rgb; public final String cls; public final int nonblackPct, colors;
        /** The size of the picture the runner copied from (before it made the small copy): the machine's own width x height. */
        public int srcW, srcH;
        Frame(int w, int h, byte[] rgb, String cls, int pct, int colors) { this.w = w; this.h = h; this.rgb = rgb; this.cls = cls; this.nonblackPct = pct; this.colors = colors; }
    }
    private static final java.util.regex.Pattern FRAME = java.util.regex.Pattern.compile("^ui_runner_vm_clone FRAME src=(\\d{1,5})x(\\d{1,5}) out=(\\d{1,3})x(\\d{1,3}) class=([a-z-]{1,40}) nonblack_pct=(\\d{1,3}) colors=(\\d{1,5}) estimate=pixel-statistics-only data=([0-9a-f]+)$");
    /** The frame in a runner output line, or null (not a frame line, or a malformed one). */
    static Frame parseFrame(String line) {
        java.util.regex.Matcher m = FRAME.matcher(line);
        if (!m.matches()) return null;
        int w = Integer.parseInt(m.group(3)), h = Integer.parseInt(m.group(4));
        String hex = m.group(8);
        if (w < 1 || w > 96 || h < 1 || h > 64 || hex.length() != w * h * 6) return null;
        byte[] rgb = new byte[w * h * 3];
        for (int i = 0; i < rgb.length; i++) rgb[i] = (byte) Integer.parseInt(hex.substring(2 * i, 2 * i + 2), 16);
        Frame fr = new Frame(w, h, rgb, m.group(5), Integer.parseInt(m.group(6)), Integer.parseInt(m.group(7)));
        fr.srcW = Integer.parseInt(m.group(1)); fr.srcH = Integer.parseInt(m.group(2));
        return fr;
    }
    private static final java.util.regex.Pattern VERDICT = java.util.regex.Pattern.compile("^ui_runner_vm_clone VERDICT: vm_stop_confirmed=([01]) stop_kind=([a-z-]{1,24}) disk_flush=(ok|FAILED|not-attempted) macos_boot=not-determined.*$");

    public static final class Run {
        volatile G7FrameFile.Info frameFile;   // newest FRAMEFILE answer (T9); a new object per answer
        public G7FrameFile.Info frameFile() { return frameFile; }
        volatile Frame frame; volatile String verdictLine, frameNote = "아직 러너에 화면을 요청하지 않았습니다."; volatile long frameAnsweredMs;
        final G7CloneMonitor mon = new G7CloneMonitor();
        public G7CloneMonitor monitor() { return mon; }
        final Process proc; final String logName; final G7BootStage.Tracker stages = new G7BootStage.Tracker();
        private final StringBuilder tail = new StringBuilder();
        private volatile boolean finished; private volatile int exitCode = Integer.MIN_VALUE; private volatile boolean cancelled;
        private final long t0 = System.currentTimeMillis();
        private OutputStream log; private int written;
        Run(Process p, String logName) { this.proc = p; this.logName = logName; }
        public boolean finished() { return finished; }
        public boolean wasCancelled() { return cancelled; }
        /** The parsed VERDICT line, or null (no line, or one that does not parse). */
        public G7RunVerdict parsedVerdict() { return G7RunVerdict.parse(verdictLine); }
        public int exitCode() { return exitCode; }
        public String logName() { return logName; }
        public synchronized String tail() { return tail.toString(); }
        public String stageSummary() { return stages.summary(); }
        public Frame frame() { return frame; }
        /** What the runner last said about a picture: 'no picture yet' with the reason, or the class of the last one. */
        public String frameNote() { return frameNote; }
        /** Asks the runner for a picture now (it answers with one FRAME line). */
        /** TEMPORARY guest-input test (research/g7-input): the last lines of the injection trail, newest last. Both the app's own steps ("app: ...") and the runner's INJECT lines. */
        private final java.util.ArrayDeque<String> injectLog = new java.util.ArrayDeque<>();
        public synchronized void injectNote(String s) { injectLog.addLast(s); while (injectLog.size() > 80) injectLog.removeFirst(); }
        /** The runner's last "INJECT-CMD ... -> REJECTED: ..." answer (null = none yet): the input could not reach the guest. */
        public volatile String injectRejected;
        public synchronized String injectTail() { StringBuilder b = new StringBuilder(); for (String l : injectLog) b.append(l).append('\n'); return b.toString(); }
        /** Sends one injection command (only the lines G7InputMap.allowed accepts) to the runner's stdin. Returns null when it was written, else why not. The runner's own answer comes back as an INJECT-CMD line. */
        public String sendInject(String line) {
            if (!G7InputMap.allowed(line)) { injectNote("app: refused (not an injection command): " + line); return "허용되지 않는 명령"; }
            if (finished) { injectNote("app: refused, the run has ended: " + line); return "실행이 끝났습니다"; }
            try { OutputStream o = proc.getOutputStream(); o.write((line + "\n").getBytes(StandardCharsets.US_ASCII)); o.flush(); }
            catch (IOException e) { injectNote("app: write to the runner failed: " + e.getMessage()); return "러너에 쓰지 못했습니다"; }
            injectNote("app: sent to the runner's input: '" + line + "' (written to its stdin only, NOT a delivery claim; the runner answers with INJECT-CMD queued/REJECTED/refused, then INJECT lines)");
            return null;
        }
        public void requestFrame() {
            if (finished) return;
            try { OutputStream o = proc.getOutputStream(); o.write("frame\n".getBytes(StandardCharsets.US_ASCII)); o.flush(); } catch (IOException ignored) { }
        }
        /** What the end of the run says, with the two things kept apart: whether the machine's stop was confirmed (and who ended it), and macOS booting (never decided here). */
        public String verdict() {
            String v = verdictLine;
            if (v == null) return "러너가 결과 줄을 남기지 않고 끝났습니다(종료 코드 " + exitCode + "). VM 종료 확인 없음, 원인 미확인(비정상 종료 가능). macOS 부팅 성공 여부는 판정하지 않았습니다.";
            java.util.regex.Matcher m = VERDICT.matcher(v);
            if (!m.matches()) return "러너의 결과 줄을 해석하지 못했습니다. macOS 부팅 성공 여부는 판정하지 않았습니다.";
            String kind = m.group(2), who = kind.equals("guest-initiated") ? "게스트가 스스로 멈춤" : kind.equals("time-limit") ? "시험 시간 제한" : kind.equals("requested-quit") ? "사용자 취소/종료 요청" : kind.equals("signal") ? "신호로 종료" : kind.equals("app-closed") ? "앱이 닫음" : kind.equals("engine-fatal") ? "엔진 치명 오류(번역할 수 없는 블록, VM 전체를 멈춤)" : "입력 오류";
            String fl = m.group(3).equals("ok") ? "성공" : m.group(3).equals("FAILED") ? "실패(디스크에 모두 반영됐다고 볼 수 없음)" : "시도하지 않음";
            G7RunVerdict rv = G7RunVerdict.parse(v);
            String more = "";
            if (rv != null && rv.structured) {
                String oc = rv.isRestart() ? "게스트가 재시작을 요청함(SYSTEM_RESET)" : rv.outcome.equals("shutdown") ? "게스트가 종료를 요청함(SYSTEM_OFF)" : rv.outcome.equals("halt-other") ? "PSCI 기록 없는 정지(정지 주소 등, 원인 미확인)" : rv.outcome.equals("host-stop") ? "호스트가 멈춤" : rv.outcome.equals("engine-fatal") ? "엔진 치명 오류(번역 실패로 엔진이 VM 전체를 멈춤, 로그의 ENGINE FATAL 줄 참조, 종료 코드 4)" : "종료 미확인";
                more = " 결과: " + oc + ", 가동 " + rv.uptimeS + "초, CPU 스레드·디스크 작업자 정리: " + (rv.quiesced == 1 ? "완료" : rv.quiesced == 0 ? "일부 미종료" : "미보고") + ", 디스크 쓰기 " + rv.diskWrites + "건 중 거부 " + rv.diskWriteRefused + "건.";
            }
            if (rv != null && rv.structured && !rv.diskSplitText().isEmpty()) more += " " + rv.diskSplitText();   // root and AUX counted apart (new runners only); counts, not a persistence claim
            return "VM 종료 확인: " + (m.group(1).equals("1") ? "예" : "아니오(강제 종료)") + ", 디스크 flush: " + fl + "(종료 확인과 별개), 종료 계기: " + who + "." + more + " macOS 부팅 성공 여부는 판정하지 않았습니다(추정 단계: " + stages.summary() + ").";
        }
        private static final java.util.regex.Pattern PID = java.util.regex.Pattern.compile("g7-process: runner_pid=(\\d{1,10})");
        /** The runner's own pid (from its "g7-process: runner_pid=" line), 0 until that line has been seen. */
        volatile int runnerPid;
        /** One line of the app side (G7RunService) into the same log, so one file shows what the app did next to what the runner did. */
        public void note(String text) { put("g7-service: " + text + "\n", true); }
        synchronized void put(String text, boolean always) {
            java.util.regex.Matcher pm = PID.matcher(text);
            if (pm.find()) { try { runnerPid = Integer.parseInt(pm.group(1)); } catch (NumberFormatException ignored) { } }
            tail.append(text); if (tail.length() > 6000) tail.replace(0, tail.length() - 6000, "");
            if (log == null) return;
            byte[] b = text.getBytes(StandardCharsets.UTF_8);
            if (!always && written + b.length > MAX_LOG_BYTES) return;
            try { log.write(b); written += b.length; log.flush(); } catch (IOException ignored) { /* the screen tail still has it */ }
        }
        /** Asks the runner to stop on its own ("quit" on its input); if it is still there after STOP_WAIT_MS it is ended. */
        public void cancel() {
            cancelled = true;
            new Thread(() -> {
                try { OutputStream o = proc.getOutputStream(); o.write("quit\n".getBytes(StandardCharsets.US_ASCII)); o.flush(); } catch (IOException ignored) { }
                try { if (!proc.waitFor(STOP_WAIT_MS, TimeUnit.MILLISECONDS)) proc.destroy(); } catch (InterruptedException ignored) { }
            }, "g7-cboot-cancel").start();
        }
    }

    /** USB test modes of the runner's --usb-mode: 0 = A none (default), 1 = B virtual USB device port only, 2 = C port + non-destructive enumeration, 3 = D = C plus a read-only getenv of five recovery variables, 4 = E = D plus the fixed auto-boot true + saveenv sequence (clone-001 only). installer stays 0 in all of them. */
    public static final int USB_NONE = 0, USB_PORT = 1, USB_PORT_PROBE = 2, USB_PORT_GETENV = 3, USB_AUTOBOOT_FIX = 4;

    public static Start start(File nativeLibDir, File extVmDir, File priv, File filesDir, File cloneDir, boolean consent, IntSupplier runnerStatus) {
        return start(nativeLibDir, extVmDir, priv, filesDir, cloneDir, consent, runnerStatus, USB_NONE);
    }

    public static Start start(File nativeLibDir, File extVmDir, File priv, File filesDir, File cloneDir, boolean consent, IntSupplier runnerStatus, int usbMode) {
        return start(nativeLibDir, extVmDir, priv, filesDir, cloneDir, consent, runnerStatus, usbMode, RUN_SECONDS);
    }

    public static Start start(File nativeLibDir, File extVmDir, File priv, File filesDir, File cloneDir, boolean consent, IntSupplier runnerStatus, int usbMode, int seconds) {
        return startWith(REAL, G7ProbeLauncher::usableRomFile, G7ExecLauncher.START_ENABLED, G7ExecLauncher::hasLiveRun, nativeLibDir, extVmDir, priv, filesDir, cloneDir, consent, runnerStatus, usbMode, seconds);
    }

    static Start startWith(Spawner sp, G7ExecLauncher.RomFinder roms, boolean enabled, BooleanSupplier g7Live, File nativeLibDir, File extVmDir, File priv, File filesDir, File cloneDir, boolean consent, IntSupplier runnerStatus) {
        return startWith(sp, roms, enabled, g7Live, nativeLibDir, extVmDir, priv, filesDir, cloneDir, consent, runnerStatus, USB_NONE);
    }

    static Start startWith(Spawner sp, G7ExecLauncher.RomFinder roms, boolean enabled, BooleanSupplier g7Live, File nativeLibDir, File extVmDir, File priv, File filesDir, File cloneDir, boolean consent, IntSupplier runnerStatus, int usbMode) {
        return startWith(sp, roms, enabled, g7Live, nativeLibDir, extVmDir, priv, filesDir, cloneDir, consent, runnerStatus, usbMode, RUN_SECONDS);
    }

    static Start startWith(Spawner sp, G7ExecLauncher.RomFinder roms, boolean enabled, BooleanSupplier g7Live, File nativeLibDir, File extVmDir, File priv, File filesDir, File cloneDir, boolean consent, IntSupplier runnerStatus, int usbMode, int seconds) {
        if (cloneDir == null) return new Start(false, "복제본 폴더가 지정되지 않아 시작하지 않았습니다. (원본 직접 부팅은 별도 버튼과 별도 동의가 필요합니다.)", null);
        return startCore(sp, roms, enabled, g7Live, nativeLibDir, extVmDir, priv, filesDir, cloneDir, false, consent, runnerStatus, usbMode, seconds);
    }

    /** Boots the RESTORED ORIGINAL install pair (state=done) itself, with no copy: for a device whose free space cannot hold one. It writes the original disk and AUX, so it has its own typed consent
     *  (the caller passes true only after the person confirmed that this run changes the original and that there is no clean copy to go back to), is refused while anything else is running, and never uses USB mode E. */
    public static Start startDirect(File nativeLibDir, File extVmDir, File priv, File filesDir, boolean directConsent, IntSupplier runnerStatus, int usbMode, int seconds) {
        return startDirectWith(REAL, G7ProbeLauncher::usableRomFile, G7ExecLauncher.START_ENABLED, G7ExecLauncher::hasLiveRun, nativeLibDir, extVmDir, priv, filesDir, directConsent, runnerStatus, usbMode, seconds);
    }
    static Start startDirectWith(Spawner sp, G7ExecLauncher.RomFinder roms, boolean enabled, BooleanSupplier g7Live, File nativeLibDir, File extVmDir, File priv, File filesDir, boolean directConsent, IntSupplier runnerStatus, int usbMode, int seconds) {
        if (usbMode == USB_AUTOBOOT_FIX) return new Start(false, "E 모드(auto-boot 켜기)는 원본 직접 부팅에서 허용되지 않습니다. 시작하지 않았습니다.", null);
        return startCore(sp, roms, enabled, g7Live, nativeLibDir, extVmDir, priv, filesDir, null, true, directConsent, runnerStatus, usbMode, seconds);
    }

    private static Start startCore(Spawner sp, G7ExecLauncher.RomFinder roms, boolean enabled, BooleanSupplier g7Live, File nativeLibDir, File extVmDir, File priv, File filesDir, File cloneDir, boolean direct, boolean consent, IntSupplier runnerStatus, int usbMode, int seconds) {
        if (!validSeconds(seconds)) return new Start(false, "허용되지 않은 실행 시간 제한이라 시작하지 않았습니다.", null);
        final boolean exactNow = statsExact;   // the log header; lagfxEnv reads the same switch when the process is spawned
        final int peakNow = statsExactPeakMib;   // same: the log header and the environment use this value
        if (exactNow && !validStatsExactPeak(peakNow)) return new Start(false, "허용되지 않은 정확 통계 메모리 한도(" + peakNow + " MiB)라 시작하지 않았습니다.", null);
        final int jit = jitMb;   // one read: the check, the command line and the log header all use this value
        if (!validJitMb(jit)) return new Start(false, "허용되지 않은 JIT 크기(" + jit + " MiB)라 시작하지 않았습니다.", null);
        if (usbMode < USB_NONE || usbMode > USB_AUTOBOOT_FIX) return new Start(false, "알 수 없는 USB 모드라 시작하지 않았습니다.", null);
        if (usbMode == USB_AUTOBOOT_FIX && (direct || cloneDir == null || !"clone-001".equals(cloneDir.getName()))) return new Start(false, "E 모드(auto-boot 켜기)는 clone-001에서만 허용됩니다. 시작하지 않았습니다.", null);
        synchronized (G7ExecLauncher.START_LOCK) {
            String g = G7Isolation.gate(enabled, g7Live, runnerStatus);
            if (g != null) return new Start(false, g.replace("격리하지", "부팅하지").replace("아무것도 옮기지", "아무것도 시작하지"), null);
            if (!consent) return new Start(false, direct ? "원본 직접 부팅 동의가 체크되지 않아 시작하지 않았습니다." : "동의가 체크되지 않아 복제본 부팅을 시작하지 않았습니다.", null);
            if (G7Isolation.pending(priv)) return new Start(false, "끝나지 않은 격리가 있어 부팅하지 않았습니다.", null);
            if (!isPackaged(nativeLibDir)) return new Start(false, "복제본 부팅 실행 파일(" + LIB_NAME + ")이 이 빌드에 없어 시작하지 않았습니다.", null);
            File rom = roms.find(extVmDir, priv);
            long ecid = G7ProbeLauncher.readEcid(priv);
            if (rom == null || ecid <= 0) return new Start(false, "ROM 또는 ECID를 확인하지 못해 시작하지 않았습니다.", null);
            File cdir = null, pdir;
            try { cdir = direct ? null : cloneDir.getCanonicalFile(); pdir = priv.getCanonicalFile(); } catch (IOException e) { return new Start(false, "경로를 확인하지 못해 시작하지 않았습니다.", null); }
            String bad = direct ? G7Clone.validateOriginal(pdir, ecid) : G7Clone.validate(cdir, pdir, ecid);
            if (bad != null) return new Start(false, (direct ? "원본 설치 쌍으로 부팅하지 않았습니다: " : "이 복제본으로 부팅하지 않았습니다: ") + bad, null);
            File log = new File(filesDir, LOG_PREFIX + String.format("%013d", System.currentTimeMillis()) + LOG_SUFFIX);
            vmLogName = log.getName();
            OutputStream logOut;
            try { logOut = Files.newOutputStream(log.toPath(), StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE); }
            catch (IOException e) { return new Start(false, "로그 파일을 열지 못해 시작하지 않았습니다.", null); }
            List<String> cmd = new ArrayList<>();
            cmd.add(new File(nativeLibDir, LIB_NAME).getPath());
            cmd.add("--shm"); cmd.add(new File(filesDir, "g7_cloneboot.shm").getPath());
            cmd.add("--rom"); cmd.add(rom.getPath());
            if (direct) cmd.add("--direct"); else { cmd.add("--clone-dir"); cmd.add(cdir.getPath()); }
            cmd.add("--ecid-dir"); cmd.add(pdir.getPath());   // plain canonical paths: the runner refuses anything else
            cmd.add("--seconds"); cmd.add(String.valueOf(seconds));   // 0 = no limit (the runner then waits for the stop request, a guest end or a fatal error)
            cmd.add("--ram-mb"); cmd.add("2048"); cmd.add("--cpus"); cmd.add("2"); cmd.add("--jit-mb"); cmd.add(String.valueOf(jit));   // validated against JIT_MB_CHOICES above; default 256
            cmd.add("--usb-mode"); cmd.add(String.valueOf(usbMode));
            lastRunConfig = (direct ? "direct=original" : "clone=" + cloneDir.getName()) + "; usb_mode=" + usbMode;   // log text only: lets the export read the run's own options
            Process p;
            try { p = sp.spawn(cmd); }
            catch (IOException e) { try { logOut.close(); } catch (IOException ignored) { } return new Start(false, "실행 파일을 시작하지 못했습니다: " + G7ExecLauncher.mask(String.valueOf(e.getMessage()), ecid), null); }
            final Run run = new Run(p, log.getName());
            run.log = logOut;
            G7ExecLauncher.OTHER_LIVE = G7CloneBoot::hasLive;
            CURRENT = run;
            watch(run, p, ecid, jit, exactNow, peakNow);
            return new Start(true, direct ? "원본 설치 쌍을 직접 부팅하기 시작했습니다. 이 실행은 원본 디스크와 AUX에 기록합니다." : "복제본(" + cloneDir.getName() + ") 부팅을 시작했습니다. 원본 설치 쌍은 사용하지 않습니다.", run);
        }
    }

    private static void watch(final Run run, final Process p, final long ecid, final int jit, final boolean exact, final int peakNow) {
        final Thread reader = new Thread(() -> {
            try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
                run.put("g7-cboot-start: " + System.currentTimeMillis() + "\n", true);
                run.put("g7-gpu-options: " + lastGpuNote + "\n", true);
                run.put("g7-run-config: " + lastRunConfig + "\n", true);
                run.put("g7-jit-config: jit_mb=" + jit + (jit == JIT_MB_DEFAULT ? " (default)" : " (chosen on the start dialog; runner --jit-mb " + jit + ")") + " jit_exact=" + (exact ? "ON" : "OFF") + " jit_exact_peak_mib=" + (exact ? statsExactPeakName(peakNow) : "n/a") + "\n", true);
                run.put(engineDiagLine(exact), true);
                String line;
                while ((line = r.readLine()) != null) {
                    line = G7ExecLauncher.mask(line, ecid);
                    G7BootStage.Stage s = run.stages.feed(line, System.currentTimeMillis() - run.t0);
                    if (line.startsWith("ui_runner_vm_clone FRAMEFILE ")) {   // integ7b T9: the full-resolution picture is a FILE in the app's files folder (host/g7_frame_file.h); this short line only says it is there. Not the hex path.
                        G7FrameFile.Info fi = G7FrameFile.parseLine(line);
                        if (fi != null) { run.put("g7-frame-file: " + fi.w + "x" + fi.h + " seq=" + fi.seq + " (" + G7FrameFile.NAME + ", full resolution, BGRA)\n", false); run.frameFile = fi; }
                        else run.put("g7-frame-file: runner answer not usable: " + (line.length() > 160 ? line.substring(0, 160) : line) + "\n", false);
                        continue;
                    }
                    if (line.startsWith("ui_runner_vm_clone FRAME ")) {   // the picture is kept in memory for the screen, never written; the log gets a short line without the pixels
                        Frame f = parseFrame(line);
                        run.frameAnsweredMs = System.currentTimeMillis();
                        if (f != null) { run.frameNote = "마지막 응답: 화면 있음(" + f.cls + ")"; run.frame = f; run.put("g7-frame: src=" + f.srcW + "x" + f.srcH + " out=" + f.w + "x" + f.h + " class=" + f.cls + " nonblack_pct=" + f.nonblackPct + " colors=" + f.colors + " (estimate from pixel statistics only)\n", false); }
                        else { String rest = line.substring("ui_runner_vm_clone FRAME ".length()); run.frameNote = "러너 응답: 아직 보낼 화면이 없습니다(" + (rest.length() > 120 ? rest.substring(0, 120) : rest) + "). 게스트가 화면을 아직 내지 않았거나 프레임이 0개일 수 있습니다."; run.put("g7-frame: " + run.frameNote + "\n", false); }
                        continue;
                    }
                    if (line.startsWith("ui_runner_vm_clone VERDICT:")) run.verdictLine = line;
                    if (line.startsWith("ui_runner_vm_clone INJECT") || line.startsWith("ui_runner_vm_clone g7-input:")) run.injectNote(line.substring("ui_runner_vm_clone ".length()));
                    if (line.startsWith("ui_runner_vm_clone INJECT-CMD") && line.contains("-> REJECTED")) run.injectRejected = line.substring("ui_runner_vm_clone ".length());
                    run.mon.feed(line, System.currentTimeMillis());
                    run.put(line + "\n", false);
                    if (s != null) run.put("g7-stage: " + s.name().toLowerCase() + " first seen at " + (System.currentTimeMillis() - run.t0) / 1000 + "s (estimate)\n", true);
                }
            } catch (IOException ignored) { }
        }, "g7-cboot-out");
        reader.start();
        final Thread poller = new Thread(() -> {   // one status line from the runner every STATUS_EVERY_MS (its frame and disk-write counters; the screen stage comes from it)
            try {
                while (!run.finished) {
                    Thread.sleep(STATUS_EVERY_MS);
                    if (run.finished) break;
                    try { OutputStream o = p.getOutputStream(); o.write("status\nframe\n".getBytes(StandardCharsets.US_ASCII)); o.flush(); } catch (IOException e) { break; }
                }
            } catch (InterruptedException ignored) { }
        }, "g7-cboot-status");
        poller.setDaemon(true); poller.start();
        new Thread(() -> {
            int code;
            try { code = p.waitFor(); } catch (InterruptedException e) { code = -1; }
            try { reader.join(5000); } catch (InterruptedException ignored) { }
            run.exitCode = code;
            run.put("g7-cboot-exit: " + code + (run.cancelled ? " (cancelled)" : "") + "\n" + run.stages.summary() + "\ng7-cboot-verdict: " + run.verdict() + "\n", true);
            synchronized (run) { try { if (run.log != null) run.log.close(); } catch (IOException ignored) { } run.log = null; }
            run.finished = true;
            poller.interrupt();
        }, "g7-cboot-wait").start();
    }
}
