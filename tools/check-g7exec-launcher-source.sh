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
