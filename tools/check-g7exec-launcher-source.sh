#!/bin/bash
# Source rules for the app side of the G7 execution build (android/app/src/g7exec). Finds direct uses, not names built at run time.
# Usage: tools/check-g7exec-launcher-source.sh
cd "$(dirname "$0")/.." || exit 1
X=android/app/src/g7exec; XJ=$X/java/dev/muns/hengeprobe; MAIN=android/app/src/main
L=$XJ/G7ExecLauncher.java; A=$XJ/G7ExecActivity.java
fail=0; bad() { echo "FAIL $*"; fail=1; }
[ -f "$L" ] && [ -f "$A" ] && [ -f "$X/AndroidManifest.xml" ] || { echo "FAIL missing launcher, activity or manifest"; exit 1; }
# the start switch comes only from the build type (BuildConfig): OFF in g7exec, ON only in g7start; the only public start passes exactly that constant
G=android/app/build.gradle
grep -qE '^\s*static final boolean START_ENABLED = BuildConfig\.G7_START_ENABLED;' "$L" || bad "START_ENABLED must be BuildConfig.G7_START_ENABLED (no literal true, no other source)"
[ "$(grep -c 'START_ENABLED' "$L" | tr -d ' ')" -le 7 ] || bad "START_ENABLED is used in too many places"
grep -q 'return startWith(REAL, G7ProbeLauncher::usableRomFile, START_ENABLED,' "$L" || bad "start() must pass the START_ENABLED constant"
[ "$(grep -c "G7_START_ENABLED" $G)" = 2 ] || bad "build.gradle must set G7_START_ENABLED exactly twice (g7exec false, g7start true)"
awk '/^        g7exec \{/{k="exec"} /^        g7start \{/{k="start"} /G7_START_ENABLED/{print k, $0}' $G > /tmp/g7sw.$$
grep -q "^exec .*'G7_START_ENABLED', 'false'" /tmp/g7sw.$$ || bad "the g7exec build type must set G7_START_ENABLED to false"
grep -q "^start .*'G7_START_ENABLED', 'true'" /tmp/g7sw.$$ || bad "the g7start build type must set G7_START_ENABLED to true"
rm -f /tmp/g7sw.$$
x=$(grep -rln "G7_START_ENABLED\|BuildConfig.G7_" android/app/src --include=*.java --include=*.xml | grep -v "^$XJ/G7Exec"); [ -z "$x" ] || { bad "the start switch is read outside the G7 execution classes:"; echo "$x"; }
x=$(grep -rn "G7_START_ENABLED" android/app/src/main 2>/dev/null); [ -z "$x" ] || { bad "the general source set names the start switch"; echo "$x"; }
[ "$(grep -c 'startWith(REAL' "$L")" = 1 ] || bad "REAL spawner is used in more than one place"
# one process creation, only here
n=$(grep -c 'new ProcessBuilder' "$L"); [ "$n" = 1 ] || bad "exactly one ProcessBuilder in the launcher (found $n)"
x=$(grep -rnE 'ProcessBuilder|Runtime\.getRuntime|\.exec\(' android/app/src --include=*.java | grep -E 'G7Exec' | grep -v "^$L:"); [ -z "$x" ] || { bad "process creation in the G7 execution activity:"; echo "$x"; }
# the binary and the launcher are named only in the g7exec source set
x=$(grep -rln 'libg7runner\|G7ExecLauncher\|G7ExecActivity\|g7exec' $MAIN 2>/dev/null); [ -z "$x" ] || { bad "the main source set names the G7 execution build:"; echo "$x"; }
grep -q 'LIB_NAME = "libg7runner.so"' "$L" || bad "LIB_NAME must be libg7runner.so"
# the ECID never travels on a command line and is never printed
x=$(grep -nE '"--ecid"|--ecid ' "$L"); [ -z "$x" ] || { bad "an --ecid option:"; echo "$x"; }
grep -q 'mask(' "$L" || bad "output must pass through mask()"
# the options: exactly the ones the runner needs; nothing that lowers a check
x=$(grep -nE '"--(installer|seed|aux-offset|min|force|skip|allow)' "$L"); [ -z "$x" ] || { bad "an option the runner must not get:"; echo "$x"; }
# nothing here deletes, moves or resets an install file
x=$(grep -nE '\.delete\(|renameTo|Files\.(delete|move)|RandomAccessFile\(.*"rw"|hv_install_disk' "$L" "$A"); [ -z "$x" ] || { bad "a delete/move/rewrite path:"; echo "$x"; }
# writes: only a NEW run log per run (CREATE_NEW: never appended to, never overwritten) and the work directory
[ "$(grep -c 'Files.newOutputStream' "$L")" = 1 ] || bad "exactly one output stream (the run log)"
grep -q 'Files.newOutputStream(log.toPath(), StandardOpenOption.CREATE_NEW' "$L" || bad "the run log must be opened with CREATE_NEW"
x=$(grep -nE 'FileOutputStream|FileWriter|StandardOpenOption.(APPEND|TRUNCATE_EXISTING)' "$L"); [ -z "$x" ] || { bad "a way to append to or overwrite a file:"; echo "$x"; }
# the run state is process-wide (not in the Activity)
grep -q 'static volatile Run CURRENT' "$L" || bad "the run state must be process-wide (CURRENT)"
x=$(grep -nE 'G7ExecLauncher\.Run [a-z]+;|private .*Run run' "$A"); [ -z "$x" ] || { bad "the activity keeps run state in a field:"; echo "$x"; }
x=$(grep -nE 'FileOutputStream|FileWriter|Files\.write|createNewFile|openFileOutput' "$A"); [ -z "$x" ] || { bad "the activity writes files:"; echo "$x"; }
# the clone-boot launcher (G7CloneBoot): its own single process creation, the same start switch, no ECID on a command line, never names the original pair's files, writes only its own new log
CB=$XJ/G7CloneBoot.java
[ -f "$CB" ] || bad "G7CloneBoot.java is missing"
[ "$(grep -c 'new ProcessBuilder' "$CB")" = 1 ] || bad "exactly one ProcessBuilder in G7CloneBoot"
PR=android/app/src/g7exec/java/dev/muns/hengeprobe/G7PipeRepro.java
[ "$(grep -c 'new ProcessBuilder' "$PR")" = 1 ] || bad "exactly one ProcessBuilder in G7PipeRepro"
grep -q 'LIB_NAME = "libg7pipelinerepro.so"' "$PR" || bad "G7PipeRepro LIB_NAME must be libg7pipelinerepro.so"
grep -qE 'Runtime\.getRuntime|\.exec\(|delete\(|renameTo|Files\.(delete|move)' "$PR" && bad "G7PipeRepro must not delete, move or use Runtime.exec"
grep -q 'G7PipeRepro.cancelActive(' "$A" || bad "the activity must kill a running reproduction (child process tree) in onDestroy"
grep -q 'static String killTree(Process p)' "$PR" && grep -q 'ROW_GEN_VERSION' "$PR" && grep -q 'countsAsDone' "$PR" || bad "G7PipeRepro lost the verified tree kill, the row-generator version or the resume rule"
grep -q 'return startWith(REAL, G7ProbeLauncher::usableRomFile, G7ExecLauncher.START_ENABLED, G7ExecLauncher::hasLiveRun,' "$CB" || bad "G7CloneBoot.start() must pass the START_ENABLED constant and the live-run check"
[ "$(grep -c 'startWith(REAL' "$CB")" = 1 ] || bad "G7CloneBoot: REAL spawner is used in more than one place"
grep -q 'LIB_NAME = "libg7cloneboot.so"' "$CB" || bad "G7CloneBoot LIB_NAME must be libg7cloneboot.so"
grep -q 'G7Clone.validate(' "$CB" || bad "G7CloneBoot must validate the clone before it starts"
grep -q 'G7Isolation.gate(' "$CB" || bad "G7CloneBoot must pass the isolation gate"
grep -q 'synchronized (G7ExecLauncher.START_LOCK)' "$CB" || bad "G7CloneBoot must hold the start lock"
x=$(grep -nE '"--ecid"|--ecid |"--installer|"--seed|macos_disk|macos_aux|"--readonly|\.delete\(|renameTo|Files\.(delete|move|copy)|FileOutputStream|FileWriter|RandomAccessFile' "$CB"); [ -z "$x" ] || { bad "G7CloneBoot: a forbidden option or file operation:"; echo "$x"; }
grep -q 'cmd.add("--usb-mode"); cmd.add(String.valueOf(usbMode))' "$CB" || bad "G7CloneBoot must pass the validated usb mode to the runner"
grep -q 'usbMode < USB_NONE || usbMode > USB_AUTOBOOT_FIX' "$CB" || bad "G7CloneBoot must reject an unknown usb mode before anything starts"
grep -q 'usbMode == USB_AUTOBOOT_FIX && (cloneDir == null || !"clone-001".equals(cloneDir.getName()))' "$CB" || bad "G7CloneBoot must limit mode E to clone-001"
grep -q 'getCanonicalFile()' "$CB" || bad "G7CloneBoot must pass canonical paths to the runner"
grep -q 'mask(' "$CB" || bad "G7CloneBoot output must pass through mask()"
[ "$(grep -c 'Files.newOutputStream' "$CB")" = 1 ] && grep -q 'Files.newOutputStream(log.toPath(), StandardOpenOption.CREATE_NEW' "$CB" || bad "G7CloneBoot: exactly one output stream, the run log, CREATE_NEW"
# the activity: no intent input, no logging, starts only through the launcher
x=$(grep -nE 'getIntent|getExtras|getStringExtra|Log\.[a-z]\(' "$A" "$L"); [ -z "$x" ] || { bad "intent input or logging:"; echo "$x"; }
grep -q 'G7ExecLauncher.start(' "$A" || bad "the activity must start only through G7ExecLauncher.start"
# nothing is typed: no input field in the activity; the consent is a checked box passed as a boolean; the IPSW SHA-256 is the one built-in reference (64 lower-case hex digits), never an input
x=$(grep -nE 'EditText|getText\(\)\.toString|APPROVAL_PHRASE|approvalText' "$A" "$L"); [ -z "$x" ] || { bad "a typed input or the old approval phrase is still there:"; echo "$x"; }
grep -qE 'static final String IPSW_SHA256 = "[0-9a-f]{64}";' "$L" || bad "IPSW_SHA256 must be one built-in 64 lower-case hex literal"
grep -q 'G7ExecLauncher.start(nativeDir, ext, priv, getFilesDir(), consent.isChecked())' "$A" || bad "the activity must pass the checkbox state to start()"
grep -q 'CheckBox' "$A" || bad "the activity needs the consent check box"
# the log provider: read-only, not exported, only run logs by plain name
P=$XJ/G7LogProvider.java
[ -f "$P" ] || bad "the log provider is missing"
grep -q 'android:exported="false"' $X/AndroidManifest.xml && grep -q 'G7LogProvider' $X/AndroidManifest.xml || bad "the log provider must be declared and not exported"
[ "$(grep -c '<provider' $X/AndroidManifest.xml)" = 1 ] || bad "the g7exec manifest declares exactly one provider"
x=$(grep -nE 'exported="true"' $X/AndroidManifest.xml | grep -v 'G7ExecActivity'); [ "$(grep -c 'exported="true"' $X/AndroidManifest.xml)" = 1 ] || bad "only the activity may be exported"
grep -q 'G7ExecLauncher.logFile(' "$P" || bad "the provider must go through logFile() (plain run-log names only)"
x=$(grep -nE 'MODE_(WRITE|READ_WRITE|APPEND|TRUNCATE|CREATE)|FileOutputStream|\.delete\(|renameTo' "$P"); [ -z "$x" ] || { bad "the provider can write or remove:"; echo "$x"; }
grep -q 'MODE_READ_ONLY' "$P" || bad "the provider must open files read-only"
# the isolation class: rename only (one atomic move, never replacing), no delete/copy/write of any file content
I=$XJ/G7Isolation.java
x=$(grep -nE '\.delete\(|renameTo|Files\.delete|REPLACE_EXISTING|Files\.copy|FileOutputStream|FileWriter|RandomAccessFile|deleteOnExit|\.truncate' "$I"); [ -z "$x" ] || { bad "the isolation class deletes, replaces, copies or rewrites:"; echo "$x"; }
[ "$(grep -c 'Files\.move(' "$I")" = 1 ] || bad "the isolation class has exactly one Files.move"
grep -q 'ATOMIC_MOVE' "$I" || bad "the isolation move must be ATOMIC_MOVE"
grep -q 'START_LOCK' "$I" || bad "the isolation must hold the start lock"
# the lagfx run must keep writing its own log file (g7_lagfx_*.txt): lagfxEnv has to set LAGFX_LOG_FILE and LAGFX_LOG_LEVEL
grep -q 'env.put("LAGFX_LOG_FILE"' $XJ/G7CloneBoot.java || bad "G7CloneBoot.lagfxEnv no longer sets LAGFX_LOG_FILE"
grep -q 'env.put("LAGFX_LOG_LEVEL"' $XJ/G7CloneBoot.java || bad "G7CloneBoot.lagfxEnv no longer sets LAGFX_LOG_LEVEL"
# the GPU-exec checkbox is remembered (SharedPreferences, default OFF) and its state is on the dialog and in the g7-gpu-options line
grep -q 'getBoolean(KEY_GPU_EXEC, false)' $A || bad "the GPU-exec checkbox must be restored from SharedPreferences with default false"
grep -q 'putBoolean(KEY_GPU_EXEC, gx.isChecked())' $A || bad "the GPU-exec checkbox must be saved when a start is requested"
grep -q 'note.append("gpu_exec=" + (gpuExec ? "ON" : "OFF")' $XJ/G7CloneBoot.java || bad "g7-gpu-options must state gpu_exec=ON/OFF"
# Render Replay capture: remembered, default OFF, passed only as LAGFX_ARM64_DUMP_DATA=1 and only with gpu exec and the dump folder
grep -q 'getBoolean(KEY_REPLAY, false)' $A && grep -q 'putBoolean(KEY_REPLAY, cdc.isChecked())' $A || bad "the Render Replay checkbox must be restored (default false) and saved"
[ "$(grep -c 'env.put("LAGFX_ARM64_DUMP_DATA"' $XJ/G7CloneBoot.java)" = 1 ] && grep -q 'if (captureData && gpuExec) { env.put("LAGFX_ARM64_DUMP_DATA", "1")' $XJ/G7CloneBoot.java || bad "LAGFX_ARM64_DUMP_DATA must be set in exactly one place, only for captureData && gpuExec"
grep -q 'render_replay=REQUESTED BUT NOT ACTIVE' $XJ/G7CloneBoot.java || bad "g7-gpu-options must say when Render Replay was requested but is not active"
# frame files: the engine writes them (host/vm.c, bounded, worker thread); the runner only asks for the last one and a flush, and writes nothing itself; the log line carries the source size
grep -q 'g7-frame: src=" + f.srcW + "x" + f.srcH' $XJ/G7CloneBoot.java || bad "the g7-frame log line must carry src=WxH"
grep -q 'FRAME-FILES-BEGIN' host/vm.c && grep -q 'FRAME-FILES-END' host/vm.c || bad "host/vm.c lost the FRAME-FILES region that tools/g7-frames-test.sh compiles"
grep -q 'g7_frame_%s_%ux%u.png' host/vm.c && grep -q 'boot_args_first' host/vm.c && grep -q 'frame_pvg_series("pvg_surface"' host/vm.c || bad "host/vm.c: boot_args and PVG-surface frame files must have distinct names"
grep -q 'define FRAME_MAX_FILES' host/vm.c && grep -q 'define FRAME_MAX_BYTES' host/vm.c && grep -q 'define FRAME_MAX_PX' host/vm.c || bad "host/vm.c: the frame files must stay bounded (count, bytes, pixels)"
grep -q 'hv_vm_save_last_frame();' host/ui_runner_vm_clone.c && grep -q 'hv_vm_frames_flush();' host/ui_runner_vm_clone.c || bad "the clone runner must save the last frame and flush before it stops the machine"
x=$(grep -nE 'fopen\([^)]*"[wa]|png_|fwrite|rename\(' host/ui_runner_vm_clone.c | grep -v '^[0-9]*:[[:space:]]*/[*]'); [ -z "$x" ] || { bad "the clone runner writes a file itself:"; echo "$x"; }
# per-draw diagnostics: default ON, remembered, always an explicit G7_DRAW_DIAG value (not an HV_ name), logged. The write consent is never remembered or pre-ticked.
grep -q 'getBoolean(KEY_DRAW_DIAG, true)' $A && grep -q 'putBoolean(KEY_DRAW_DIAG, dd.isChecked())' $A || bad "the draw-diag checkbox must be restored (default true) and saved"
grep -q 'env.put("G7_DRAW_DIAG", drawDiag ? "1" : "0")' $XJ/G7CloneBoot.java && grep -q 'draw_diag=' $XJ/G7CloneBoot.java || bad "lagfxEnv must pass an explicit G7_DRAW_DIAG=1/0 and log draw_diag=ON/OFF"
grep -q 'static volatile boolean drawDiag = true' $XJ/G7CloneBoot.java || bad "drawDiag must default to true"
x=$(grep -nE 'KEY_[A-Z_]+' $A | grep -iE 'consent|agree|ok\b|동의|write' | grep -v '^[0-9]*:.*//'); [ -z "$x" ] || { bad "a consent key is stored in preferences:"; echo "$x"; }
grep -q 'final CheckBox ok = shortBox(' $A && ! grep -qE 'ok\.setChecked|opts\.edit\(\)\.putBoolean\([^)]*ok\.isChecked' $A || bad "the clone write consent (ok) must stay unticked at every dialog open and never be saved"
# the storage class: the ONLY place that deletes (the person's confirmation is on the screen). One delete primitive (Files::delete, no recursion, no tree walk), no copy/move/rewrite, under the start lock and the isolation's gate
S=$XJ/G7Storage.java
x=$(grep -nE '\.delete\(|renameTo|Files\.walk|deleteRecursively|walkFileTree|REPLACE_EXISTING|Files\.copy|Files\.move|FileOutputStream|FileWriter|RandomAccessFile|deleteOnExit|\.truncate' "$S"); [ -z "$x" ] || { bad "the storage class uses a delete, tree walk, move, copy or rewrite other than its one primitive:"; echo "$x"; }
[ "$(grep -c 'Files::delete' "$S")" = 1 ] || bad "the storage class has exactly one delete primitive (Files::delete)"
[ "$(grep -c 'G7Isolation.gate(' "$S")" = 3 ] || bad "all three deletions must pass the isolation's gate"
[ "$(grep -c 'synchronized (G7ExecLauncher.START_LOCK)' "$S")" = 3 ] || bad "all three deletions must hold the start lock"
x=$(grep -nE '\.delete\(|Files\.delete|Files::delete|deleteRecursively|G7Storage\.delete'  $L $A | grep -v 'G7Storage\.delete\(Archives\|Current\|Clone\)' ); [ -z "$x" ] || { bad "something other than the storage class deletes:"; echo "$x"; }
# UI stability (research/g7-ui-fix): the live clone-boot monitor must not move the page under the person's finger; the input controls live in a fixed dock
x=$(grep -nE 'cbMon\.setTextIsSelectable|injOut\.setTextIsSelectable' "$A"); [ -z "$x" ] || { bad "a live monitor TextView is selectable again (a focused TextView scrolls the outer page on every setText):"; echo "$x"; }
grep -q 'cbMon.setFocusable(false)' "$A" && grep -q 'cbScroll.setDescendantFocusability(android.view.ViewGroup.FOCUS_BLOCK_DESCENDANTS)' "$A" || bad "the live monitor box must be non-focusable and block descendant focus"
x=$(grep -nE 'fullScroll\(' "$A"); [ -z "$x" ] || { bad "fullScroll moves focus and so scrolls the outer page; use scrollTo on the box:"; echo "$x"; }
grep -q 'cbImage.setAdjustViewBounds(false)' "$A" && ! grep -qE 'cbImage\.setMinimumHeight|cbImage\.setAdjustViewBounds\(true\)' "$A" || bad "the picture view must keep a fixed reserved height (no adjustViewBounds, no minimum-height toggling)"
grep -q 'if (text.equals(cbMonShown)) return;' "$A" || bad "the monitor text must be set only when it changed"
grep -q 'frame.addView(buildInputDock()' "$A" && grep -q 'frame.addView(sv, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))' "$A" || bad "the input controls must sit in a fixed dock outside the scrolling page"
x=$(grep -nE 'buildInputTest\(root\)' "$A"); [ -z "$x" ] || { bad "the input controls were put back into the scrolling page:"; echo "$x"; }
grep -q 'buildInputTest(inputPanel)' "$A" && grep -q 'inputScroll = new ScrollView(this); inputScroll.addView(inputPanel)' "$A" || bad "the input panel must be built into inputPanel and shown in the dock"
grep -q 'cbPause = new CheckBox' "$A" && grep -q 'if (!uiPaused || r.finished()) refreshCloneView(r);' "$A" || bad "the pause toggle must freeze the monitor/picture refresh while cbootPoll keeps polling the run state"
[ "$(grep -c 'log.postDelayed(this::cbootPoll, 2000);' "$A")" = 1 ] || bad "cbootPoll must keep rescheduling itself (the run-state poll continues while paused)"
[ "$(grep -c 'if (!uiPaused || r.finished())' "$A")" -ge 3 ] || bad "the restore, network and clone-boot polls must all honour the pause toggle"
# NVRAM diagnostics / pre-seed (research/g7-nvram1): the pre-seed toggle is OFF every time the dialog opens (never remembered), is written for EVERY start ("0" when unchecked), and the runner has no default for it
grep -q 'final CheckBox nvp = shortBox(' "$A" && ! grep -qE 'nvp\.setChecked|opts\.(getBoolean|edit\(\))[^;]*nvp' "$A" || bad "the NVRAM pre-seed checkbox must start unchecked and must not be remembered"
[ "$(grep -c 'G7Storage.setPreseedMarker(privDir, nvp.isChecked())' "$A")" = 1 ] || bad "the pre-seed marker must be written exactly once per start from the checkbox state"
grep -q '"auto-boot=true' $XJ/G7Storage.java && ! grep -q 'boot-volume=' $XJ/G7Storage.java || bad "the app side may write auto-boot=true only; boot-volume is never written from the app"
R=host/ui_runner_vm_clone.c; N=host/g7_nvram.h
[ "$(grep -c 'g7nv_preseed(' $R)" = 1 ] && grep -q 'g7nv_spec_source(getenv("G7_NVRAM_PRESEED"), getenv("HENGE_NVRAM_PRESEED")' $R || bad "the runner must request the pre-seed only through g7nv_spec_source (env or marker file), exactly one call"
l1=$(grep -n 'g7nv_preseed(ad' $R | head -1 | cut -d: -f1); l2=$(grep -n 'hv_disk_set_trace(hv_vm_aux_disk()' $R | head -1 | cut -d: -f1); l3=$(grep -n 'hv_vm_start(1, on_exit_cb)' $R | head -1 | cut -d: -f1)
{ [ -n "$l1" ] && [ -n "$l2" ] && [ -n "$l3" ] && [ "$l1" -lt "$l2" ] && [ "$l2" -lt "$l3" ]; } || bad "$R: pre-seed, then the observer, then hv_vm_start"
grep -q 'if (!src) return NULL;' $N || bad "$N: with nothing requested the pre-seed must stay off"
grep -q 'NO valid v3' $N && grep -q 'NOTHING WRITTEN' $N || bad "$N: no store found must write nothing and say so"
[ "$(grep -c 'g7nv_summary(&g_nv)' $R)" = 1 ] || bad "$R: one g7-nvram summary at stop"
# JIT size (research/g7-hjit2): the runner gets --jit-mb only from the validated set {64,128,256,384,512,768}; default 64 (what every earlier build passed); the spinner offers exactly that set, selects index 0 (64) unless the Intent extra names a member, and is never remembered
grep -q 'JIT_MB_CHOICES = {64, 128, 256, 384, 512, 768};' "$CB" || bad "G7CloneBoot.JIT_MB_CHOICES must be exactly {64, 128, 256, 384, 512, 768}"
grep -q 'JIT_MB_DEFAULT = 64;' "$CB" && grep -q 'static volatile int jitMb = JIT_MB_DEFAULT;' "$CB" || bad "the JIT size default must be 64"
grep -q 'if (!validJitMb(jit)) return new Start(false' "$CB" || bad "G7CloneBoot must refuse a JIT size outside the set before anything starts"
grep -q 'cmd.add("--jit-mb"); cmd.add(String.valueOf(jit));' "$CB" && ! grep -qE 'cmd\.add\("--jit-mb"\); cmd\.add\("[0-9]' "$CB" || bad "--jit-mb must come only from the validated value, never a literal"
[ "$(grep -c '"--jit-mb"' "$CB")" = 2 ] || bad "G7CloneBoot names --jit-mb in exactly two places (command, log-note lookup)"
grep -q 'jit_mb=' "$CB" && grep -q 'g7-jit-config: jit_mb=' "$CB" || bad "the chosen JIT size must be written to the run log header and run-info.txt"
grep -q 'run_jit_mb=' $XJ/G7ExecDumpExport.java || bad "the export manifest must record the run's JIT size"
grep -q 'new android.widget.Spinner(this); jitSp.setAdapter(new android.widget.ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, jm))' "$A" || bad "the start dialog must have the JIT size spinner built from G7CloneBoot.JIT_MB_CHOICES"
grep -q 'String\[\] jm = new String\[G7CloneBoot.JIT_MB_CHOICES.length\]' "$A" || bad "the JIT spinner values must come from JIT_MB_CHOICES (no values of its own)"
grep -q 'G7CloneBoot.jitMb = G7CloneBoot.JIT_MB_CHOICES\[' "$A" || bad "the dialog must set jitMb only to a member of JIT_MB_CHOICES"
[ "$(grep -c 'G7CloneBoot.jitMb' "$A")" = 1 ] || bad "jitMb is set in exactly one place in the activity"
grep -q 'jitSp.setSelection(jitIndexFor(G7CloneBoot.readJitMarker(privDir)))' "$A" && grep -q 'return validJitMb(v) ? v : JIT_MB_DEFAULT;' "$CB" || bad "the test marker may only preselect the spinner, and an unknown value must fall back to 64 (no Intent input is allowed)"
x=$(grep -nE 'jitSp.*(KEY_|opts\.)' "$A"); [ -z "$x" ] || { bad "the JIT size must not be remembered between starts:"; echo "$x"; }
# JIT exact stats (research/g7-hjit3): default OFF, not remembered, one validated value, never an HV_ name, the marker only preselects, recorded like jit_mb
grep -q 'static volatile boolean statsExact = false;' "$CB" || bad "G7CloneBoot: statsExact must default to OFF"
grep -q 'STATS_EXACT_VALUE = "2048";' "$CB" || bad "G7CloneBoot: the one value passed for G7_STATS_EXACT must be 2048"
[ "$(grep -c 'env.put("G7_STATS_EXACT"' "$CB")" = 1 ] && grep -q 'if (exact) env.put("G7_STATS_EXACT", STATS_EXACT_VALUE);' "$CB" || bad "G7_STATS_EXACT must be set in exactly one place, only when ON, only to STATS_EXACT_VALUE"
grep -q 'env.remove("G7_STATS_EXACT"); env.remove("G7_STATS_EXACT_PEAK_MIB");' "$CB" || bad "an inherited G7_STATS_EXACT / PEAK value must be removed first"
! grep -qE 'env\.put\("(HV_STATS_EXACT|G7_STATS_EXACT_PEAK_MIB|HV_STATS_EXACT_PEAK_MIB)"' "$CB" || bad "no HV_ name (the runner scrubs them) and no peak override may be set by the launcher"
grep -q 'return "1".equals(new String(Files.readAllBytes(p), StandardCharsets.US_ASCII).trim());' "$CB" && grep -q 'jex.setChecked(G7CloneBoot.readStatsExactMarker(privDir))' "$A" || bad "the jit_exact marker may only preselect the checkbox, and only for the content 1"
[ "$(grep -c 'G7CloneBoot.statsExact' "$A")" = 1 ] && grep -q 'G7CloneBoot.statsExact = jex.isChecked();' "$A" || bad "statsExact is set in exactly one place in the activity, from the checkbox"
! grep -qE 'jex.*(KEY_|opts\.)' "$A" || bad "the exact-stats checkbox must not be remembered"
grep -q 'jit_exact=' "$CB" && grep -q 'g7-engine-diag:' "$CB" || bad "jit_exact and the engine diagnostics must be written to the run log header and run-info.txt"
grep -q 'run_jit_exact=' $XJ/G7ExecDumpExport.java || bad "the export manifest must record the run's exact-stats state"

# F1 early framebuffer observation: default ON in the dialog (not remembered), always an explicit G7_FB0 value, the runner side only starts it on G7_FB0=1
grep -q 'static volatile boolean fb0 = true;' "$CB" && grep -q 'env.put("G7_FB0", fb0 ? "1" : "0")' "$CB" || bad "G7CloneBoot: fb0 must default to ON and always send an explicit G7_FB0 value"
grep -q 'fb0c.setChecked(true)' "$A" && ! grep -qE 'fb0c.*(KEY_|opts\.)' "$A" || bad "the fb0 checkbox must default to checked and must not be remembered"
grep -q 'if (!e || strcmp(e, "1") != 0) return;' host/vm.c && [ "$(grep -c 'g7_fb0_maybe_start();' host/vm.c)" = 1 ] || bad "host/vm.c: the g7-fb0 sampler must start only on G7_FB0=1, from exactly one place"
grep -q 'HV_FB0_MAX_DUMPS 3' host/g7_fb0.h && grep -q 'HV_FB0_PERIOD_MS 250' host/g7_fb0.h && grep -q 'HV_FB0_SECONDS 60' host/g7_fb0.h || bad "host/g7_fb0.h: the sampler's bounds (250 ms, 60 s, 3 dumps) changed"
# VBL experiment switch: default "0" = variable not set; only the three named values can ever be passed; the spinner is built from the same list and is not remembered
grep -q 'VBL_EXP_CHOICES = {"0", "tick_none", "bit3_once", "swap_no_or2"};' "$CB" && grep -q 'static volatile String vblExp = "0";' "$CB" || bad "G7CloneBoot: the VBL experiment choices must be exactly {0, tick_none, bit3_once, swap_no_or2}, default 0"
grep -q 'validVblExp(vx) && !"0".equals(vx)' "$CB" && [ "$(grep -c 'env.put("LAGFX_VBL_EXP"' "$CB")" = 1 ] || bad "G7CloneBoot: LAGFX_VBL_EXP may be set only from a validated, non-default value, in one place"
grep -q 'adapter\|setAdapter(new android.widget.ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, G7CloneBoot.VBL_EXP_CHOICES))' "$A" && ! grep -qE 'vblSp.*(KEY_|opts\.)' "$A" || bad "the VBL experiment spinner must be built from VBL_EXP_CHOICES and must not be remembered"
# the engine stats line and the runner's 10 s window line carry the JIT region size
grep -q 'of %.0f MB jit' core/dispatch.c || bad "core/dispatch.c: the stats line must show the JIT region size"
grep -q 'g7-jit: t=' host/ui_runner_vm_clone.c || bad "host/ui_runner_vm_clone.c: the g7-jit window line is missing"
# the execution manifest overlay: one activity, only the activity of this build
[ "$(grep -c '<activity' $X/AndroidManifest.xml)" = 1 ] || bad "the g7exec manifest declares exactly one activity"
# the general manifest does not mention it
grep -q 'G7Exec' $MAIN/AndroidManifest.xml && bad "the general manifest names G7Exec"
# the build type exists and is the only way to include the source set
grep -q 'g7exec {' android/app/build.gradle || bad "android/app/build.gradle lacks the g7exec build type"
# no binary is committed into the source set
x=$(find $X/jniLibs -type f ! -name .gitkeep 2>/dev/null); [ -z "$x" ] || { bad "a binary is committed under $X/jniLibs:"; echo "$x"; }
[ $fail = 0 ] && echo "OK  G7 execution launcher source rules (switch off, one process creation, no ECID on a command line, no delete/move, not in the general source set)"
exit $fail
