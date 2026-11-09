package dev.muns.hengeprobe;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.system.Os;
import android.system.OsConstants;
import android.util.TypedValue;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/** One screen: run the JIT mapping probe over growing region sizes and show the result as text. */
public class MainActivity extends Activity {
    static {
        System.loadLibrary("henge_probe");
    }

    /** One region size: map, run code at start/middle/end, sample pages, close. */
    private native String runSize(int mib, String crashPath, String progressPath);

    /** Multi-thread patch test; stage 1..5 (see mt_jni.c), seconds only matters for stages 1, 2 and 5. */
    private native String runMt(int stage, int seconds, String crashPath, String progressPath);

    /** Guest-RAM test (mem_jni.c): mode 0 sparse (size in GiB), 1 dense (size in MiB), 2 system info. type 0 anon, 1 memfd, 2 file. */
    private native String runMem(int mode, int type, int size, String filesDir, String crashPath, String progressPath);

    /** Clears the cancel request; called once before a run of stages. */
    private native void beginMt();

    /** Asks the running multi-thread test to stop at its next check; does not block. */
    private native void cancelMt();

    private static final int[] SIZES_MIB = {1, 16, 64, 256, 768};

    private TextView output;
    private Button runButton;
    private Button mtButton;
    private Button longButton;
    private Button cancelButton;
    private Button memButton;
    private Button denseButton;
    private Button reclaimButton;
    private Button engineButton;
    private Button bigVmButton;
    private Button vmrunButton;
    private File crashFile;
    private File progressFile;
    private volatile boolean running;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        crashFile = new File(getFilesDir(), "last_crash.txt");
        progressFile = new File(getFilesDir(), "progress.txt");

        int pad = (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 16, getResources().getDisplayMetrics());
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad * 2, pad, pad);

        runButton = new Button(this);
        runButton.setText("JIT 크기 시험 실행 (1~768 MiB)");
        runButton.setOnClickListener(v -> runNow());
        root.addView(runButton);

        mtButton = new Button(this);
        mtButton.setText("다중 스레드 패치 시험 (1~4단계, 약 20초)");
        mtButton.setOnClickListener(v -> runMtNow(false));
        root.addView(mtButton);

        longButton = new Button(this);
        longButton.setText("장시간 반복 (60초)");
        longButton.setOnClickListener(v -> runMtNow(true));
        root.addView(longButton);

        memButton = new Button(this);
        memButton.setText("게스트 메모리 시험 (2~8 GiB 예약, 일부 페이지)");
        memButton.setOnClickListener(v -> runMemNow(0));
        root.addView(memButton);

        denseButton = new Button(this);
        denseButton.setText("단계적 사용 시험 (종류별 1 GiB까지 실제 사용)");
        denseButton.setOnClickListener(v -> runMemNow(1));
        root.addView(denseButton);

        reclaimButton = new Button(this);
        reclaimButton.setText("메모리 반납 시험 (balloon 대체 호출)");
        reclaimButton.setOnClickListener(v -> runMemNow(2));
        root.addView(reclaimButton);

        engineButton = new Button(this);
        engineButton.setText("엔진 시험 실행 (mmu/jit/mem/exc/devices/vm)");
        engineButton.setOnClickListener(v -> runEngineNow());
        root.addView(engineButton);

        bigVmButton = new Button(this);
        bigVmButton.setText("VM 대용량 시험 (RAM 2048MB, JIT 768MB)");
        bigVmButton.setOnClickListener(v -> runBigVmNow());
        root.addView(bigVmButton);

        vmrunButton = new Button(this);
        vmrunButton.setText("실제 ROM 실행 (vmrun, 30초)");
        vmrunButton.setOnClickListener(v -> runVmrunNow());
        root.addView(vmrunButton);

        cancelButton = new Button(this);
        cancelButton.setText("시험 취소 (다중 스레드·메모리)");
        cancelButton.setOnClickListener(v -> {
            cancelMt();
            Toast.makeText(this, "취소 요청함 (멈추면 앱 강제 종료)", Toast.LENGTH_SHORT).show();
        });
        root.addView(cancelButton);

        Button copy = new Button(this);
        copy.setText("결과 복사");
        copy.setOnClickListener(v -> copyResult());
        root.addView(copy);

        output = new TextView(this);
        output.setTypeface(Typeface.MONOSPACE);
        output.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        output.setTextIsSelectable(true);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(output);
        root.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        setContentView(root);

        StringBuilder sb = new StringBuilder(deviceInfo());
        String crash = readFile(crashFile);
        String progress = readFile(progressFile);
        if (crash != null) {
            sb.append("\n이전 실행이 비정상 종료됨 (신호 기록):\n").append(crash);
        } else if (progress != null) {
            sb.append("\n이전 실행이 도중에 끝남. 마지막으로 시작한 단계 (원인은 알 수 없음):\n").append(progress);
        }
        //noinspection ResultOfMethodCallIgnored
        crashFile.delete();
        //noinspection ResultOfMethodCallIgnored
        progressFile.delete();
        sb.append("\n버튼을 누르면 1, 16, 64, 256, 768 MiB 순서로 시험합니다.\n");
        output.setText(sb.toString());
    }

    private String deviceInfo() {
        long page = -1;
        try {
            page = Os.sysconf(OsConstants._SC_PAGESIZE);
        } catch (Exception ignored) {
        }
        long build = 0;
        try {
            build = getPackageManager().getPackageInfo(getPackageName(), 0).getLongVersionCode();
        } catch (Exception ignored) {
        }
        return "빌드 번호: " + build + " (CI 실행 번호; 클수록 최신)\n기기: " + Build.MANUFACTURER + " " + Build.MODEL + "\n"
                + "Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")\n"
                + "ABI: " + Build.SUPPORTED_ABIS[0] + ", 페이지 크기: " + page + "\n";
    }

    private String readFile(File f) {
        if (!f.exists() || f.length() == 0) return null;
        try (FileInputStream in = new FileInputStream(f)) {
            byte[] data = new byte[(int) Math.min(f.length(), 4096)];
            int n = in.read(data);
            return new String(data, 0, Math.max(n, 0), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return null;
        }
    }

    private void show(String text) {
        runOnUiThread(() -> output.setText(deviceInfo() + "\n" + text));
    }

    /** The native test runs off the main thread, so a slow or stuck size cannot freeze the screen. */
    private void runNow() {
        if (running) return;
        running = true;
        runButton.setEnabled(false);
        new Thread(() -> {
            StringBuilder acc = new StringBuilder();
            boolean failed = false;
            for (int mib : SIZES_MIB) {
                show(acc + "실행 중: " + mib + " MiB ...\n");
                String r = runSize(mib, crashFile.getAbsolutePath(), progressFile.getAbsolutePath());
                acc.append(r).append('\n');
                show(acc.toString());
                if (!r.contains("RESULT: PASS")) {
                    failed = true;
                    break;
                }
            }
            //noinspection ResultOfMethodCallIgnored
            progressFile.delete();
            acc.append(failed ? "중단: 위 크기에서 실패했습니다.\n" : "모든 크기 통과 (매핑·3곳 실행·표본 페이지)\n");
            show(acc.toString());
            running = false;
            runOnUiThread(() -> runButton.setEnabled(true));
        }).start();
    }

    private void setButtons(boolean enabled) {
        runOnUiThread(() -> {
            runButton.setEnabled(enabled);
            mtButton.setEnabled(enabled);
            longButton.setEnabled(enabled);
            memButton.setEnabled(enabled);
            denseButton.setEnabled(enabled);
            reclaimButton.setEnabled(enabled);
            engineButton.setEnabled(enabled);
            bigVmButton.setEnabled(enabled);
            vmrunButton.setEnabled(enabled);
        });
    }

    /** Several threads run the patched branch while another rewrites it. Stages 1-4, or the long run (stage 5). */
    private void runMtNow(boolean longRun) {
        if (running) return;
        running = true;
        setButtons(false);
        beginMt(); // once per run of stages, so a cancel press is never lost between stages
        new Thread(() -> {
            StringBuilder acc = new StringBuilder();
            int[] stages = longRun ? new int[] {5} : new int[] {1, 2, 3, 4};
            int[] secs = longRun ? new int[] {60} : new int[] {3, 4, 0, 0};
            int fail = 0, incomplete = 0, unpinned = 0, pass = 0;
            boolean cancelled = false;
            for (int i = 0; i < stages.length && !cancelled; i++) {
                show(acc + "실행 중: 단계 " + stages[i] + " ...\n");
                String r = runMt(stages[i], secs[i], crashFile.getAbsolutePath(), progressFile.getAbsolutePath());
                acc.append(r).append('\n');
                show(acc.toString());
                if (r.contains("RESULT: PASS")) pass++;
                else if (r.contains("RESULT: CANCELLED")) cancelled = true;
                else if (r.contains("RESULT: INCOMPLETE")) incomplete++;
                else if (r.contains("RESULT: UNPINNED")) unpinned++;
                else fail++;
            }
            //noinspection ResultOfMethodCallIgnored
            progressFile.delete();
            acc.append("요약: PASS ").append(pass).append(", FAIL ").append(fail).append(", INCOMPLETE ").append(incomplete)
                    .append(", UNPINNED ").append(unpinned).append(cancelled ? ", 취소됨" : "").append('\n');
            acc.append("PASS는 이 기기·이 패치 형태에서 문제가 관찰되지 않았다는 뜻이며 증명이 아닙니다.\n");
            show(acc.toString());
            running = false;
            setButtons(true);
        }).start();
    }

    /** Guest RAM shapes: anonymous, memfd, file. kind 0 sparse (2-8 GiB), 1 dense (1 GiB really used), 2 reclaim (balloon stand-ins). */
    private void runMemNow(int kind) {
        if (running) return;
        running = true;
        setButtons(false);
        beginMt(); // clears the shared cancel request once per run
        final String dir = getFilesDir().getAbsolutePath();
        new Thread(() -> {
            StringBuilder acc = new StringBuilder();
            int pass = 0, mapFailed = 0, setupFailed = 0, unsupported = 0, skipped = 0, fail = 0;
            boolean cancelled = false;
            String[] names = {"anonymous", "memfd", "file"};
            if (kind == 0) {
                acc.append(runMem(2, 0, 0, dir, crashFile.getAbsolutePath(), progressFile.getAbsolutePath())).append('\n');
                show(acc.toString());
            }
            for (int type = 0; type < 3 && !cancelled; type++) {
                int[] sizes = kind == 0 ? new int[] {2, 4, 6, 8} : kind == 1 ? new int[] {1024} : new int[] {0};
                int mode = kind == 0 ? 0 : kind == 1 ? 1 : 3;
                for (int size : sizes) {
                    show(acc + "실행 중: " + names[type] + " ...\n");
                    String r = runMem(mode, type, size, dir, crashFile.getAbsolutePath(), progressFile.getAbsolutePath());
                    acc.append(r).append('\n');
                    show(acc.toString());
                    if (r.contains("RESULT: PASS")) pass++;
                    else if (r.contains("RESULT: CANCELLED")) {
                        cancelled = true;
                        break;
                    } else if (r.contains("RESULT: MAP_FAILED")) {
                        mapFailed++;
                        break; // larger sizes of this shape will not map either
                    } else if (r.contains("RESULT: SETUP_FAILED")) {
                        setupFailed++;
                        break;
                    } else if (r.contains("RESULT: UNSUPPORTED")) unsupported++;
                    else if (r.contains("RESULT: SKIPPED")) skipped++;
                    else fail++;
                }
            }
            //noinspection ResultOfMethodCallIgnored
            progressFile.delete();
            acc.append("요약: PASS ").append(pass).append(", MAP_FAILED ").append(mapFailed).append(", SETUP_FAILED ").append(setupFailed)
                    .append(", UNSUPPORTED ").append(unsupported).append(", SKIPPED ").append(skipped).append(", FAIL ").append(fail)
                    .append(cancelled ? ", 취소됨" : "").append('\n');
            acc.append(kind == 1
                    ? "실제 메모리를 사용한 시험입니다. 앱이 강제 종료되면 다음 실행 때 마지막 단계가 표시됩니다.\n"
                    : kind == 2
                    ? "반납 호출의 성공, 데이터, 메모리 사용량 변화는 따로 봅니다. 사용량이 줄지 않아도 실패가 아닙니다.\n"
                    : "PASS는 매핑과 표본 페이지가 동작했다는 뜻이며, 그만큼의 RAM을 쓸 수 있다는 뜻은 아닙니다.\n");
            show(acc.toString());
            running = false;
            setButtons(true);
        }).start();
    }

    /**
     * Runs the engine's own tests (built from core/ and host/ with the NDK) as separate processes. They sit in the
     * app's native library directory under lib*.so names. A separate process keeps the engine's signal handling
     * away from the app, and a crash there cannot take the app down.
     */
    private void runEngineNow() {
        runTests(new String[] {"mmu", "jit", "mem", "exc", "devices", "vm"}, null);
    }

    /** The VM test at the VM's default sizes (2048 MB RAM, 768 MB JIT): reports process memory too. */
    private void runBigVmNow() {
        runTests(new String[] {"vm"}, new String[] {"HV_TEST_BIG", "1"});
    }

    private void runTests(final String[] tests, final String[] extraEnv) {
        if (running) return;
        running = true;
        setButtons(false);
        final String libDir = getApplicationInfo().nativeLibraryDir;
        new Thread(() -> {
            StringBuilder acc = new StringBuilder();
            int ok = 0, failed = 0;
            for (String t : tests) {
                show(acc + "실행 중: test_" + t + " ...\n");
                File exe = new File(libDir, "libtest_" + t + ".so");
                acc.append("== test_").append(t).append(" ==\n");
                if (!exe.exists()) {
                    acc.append("  실행 파일 없음: ").append(exe).append("\n");
                    failed++;
                    continue;
                }
                try {
                    ProcessBuilder pb = new ProcessBuilder(exe.getAbsolutePath()).directory(getFilesDir()).redirectErrorStream(true);
                    pb.environment().put("HV_TEST_WORK", getFilesDir().getAbsolutePath()); // test_devices writes two small disk images
                    pb.environment().put("TMPDIR", getCacheDir().getAbsolutePath());
                    if (extraEnv != null) for (int e = 0; e + 1 < extraEnv.length; e += 2) pb.environment().put(extraEnv[e], extraEnv[e + 1]);
                    Process p = pb.start();
                    StringBuilder text = new StringBuilder();
                    Thread reader = new Thread(() -> {
                        try (java.io.BufferedReader br = new java.io.BufferedReader(new java.io.InputStreamReader(p.getInputStream()))) {
                            String line;
                            while ((line = br.readLine()) != null) {
                                synchronized (text) {
                                    text.append(line).append('\n');
                                }
                            }
                        } catch (IOException ignored) {
                        }
                    });
                    reader.start();
                    boolean done = p.waitFor(120, java.util.concurrent.TimeUnit.SECONDS);
                    if (!done) p.destroyForcibly();
                    reader.join(2000);
                    String out;
                    synchronized (text) {
                        out = text.toString();
                    }
                    // Only the lines that matter: failures, summaries, and anything that is not an "ok" line.
                    for (String line : out.split("\n")) {
                        if (!line.trim().endsWith(": ok")) acc.append("  ").append(line).append('\n');
                    }
                    int okLines = out.split(": ok", -1).length - 1;
                    if (okLines > 0) acc.append("  (\"ok\" 줄 ").append(okLines).append("개 생략)\n");
                    // The summary line ("N checks, M failures" and the like) must be there with N > 0 and M == 0.
                    java.util.regex.Matcher sm = java.util.regex.Pattern
                            .compile("^(\\d+) (?:checks|programs)\\b.*?(\\d+) failures", java.util.regex.Pattern.MULTILINE).matcher(out);
                    long total = -1, bad = -1;
                    while (sm.find()) {
                        total = Long.parseLong(sm.group(1));
                        bad = Long.parseLong(sm.group(2));
                    }
                    if (!done) {
                        acc.append("  RESULT: TIMEOUT (120 s)\n");
                        failed++;
                    } else if (p.exitValue() != 0) {
                        int code = p.exitValue();
                        acc.append("  RESULT: FAIL (exit code ").append(code).append(")");
                        if (code > 128) acc.append(" - 128 초과: 시그널 종료일 수 있음(").append(code - 128).append("번, 확정 아님)");
                        if (total < 0) acc.append(" - 요약 줄 없음(출력 버퍼가 비워지기 전에 종료됐을 수 있음)");
                        acc.append('\n');
                        failed++;
                    } else if (total <= 0 || bad != 0) {
                        acc.append("  RESULT: FAIL (종료 코드 0이지만 요약 줄이 없거나 검사 0건/실패 있음: 검사 ").append(total)
                                .append(", 실패 ").append(bad).append(")\n");
                        failed++;
                    } else {
                        acc.append("  RESULT: PASS (검사 ").append(total).append("건, 실패 0)\n");
                        ok++;
                    }
                } catch (Exception e) {
                    acc.append("  RESULT: FAIL (실행할 수 없음: ").append(e).append(")\n");
                    failed++;
                }
                show(acc.toString());
            }
            acc.append("요약: PASS ").append(ok).append(", FAIL ").append(failed).append('\n');
            acc.append("엔진의 번역 코드가 이 기기에서 실행된 결과입니다. 게스트 부팅과는 별개입니다.\n");
            show(acc.toString());
            running = false;
            setButtons(true);
        }).start();
    }


    /**
     * Runs host/vmrun.c on real files the user put in the app's external files dir, vm/rom.bin, disk.img, aux.img and an
     * optional config.txt ("ecid=0x..", "seed=0x..", "ram_mb=..", "cpus=..", "jit_mb=..", "seconds=..", "log=..").
     * Shows what vmrun prints: where CPU 0 is (pc, exception level, ESR/FAR) and why the machine stopped.
     */
    private void runVmrunNow() {
        if (running) return;
        running = true;
        setButtons(false);
        final String libDir = getApplicationInfo().nativeLibraryDir;
        new Thread(() -> {
            StringBuilder acc = new StringBuilder();
            try {
                File base = new File(getExternalFilesDir(null), "vm");
                File rom = new File(base, "rom.bin");
                // disk and aux: the ones in that folder if present, else blank ones in the app's private folder (made by vmrun)
                File priv = new File(getFilesDir(), "vm");
                priv.mkdirs();
                File disk = new File(base, "disk.img").exists() ? new File(base, "disk.img") : new File(priv, "blank_disk.img");
                File aux = new File(base, "aux.img").exists() ? new File(base, "aux.img") : new File(priv, "blank_aux.img");
                acc.append("폴더: ").append(base).append('\n');
                File exe = new File(libDir, "libvmrun.so");
                if (!exe.exists()) acc.append("vmrun 실행 파일이 이 APK에 없습니다.\n");
                else if (!rom.exists()) {
                    acc.append("rom.bin이 없습니다. 위 폴더에 AVPBooter ROM을 rom.bin 이름으로 넣고 다시 누르세요 ")
                            .append("(disk.img/aux.img는 없으면 빈 파일을 자동으로 만듭니다). ")
                            .append("선택: config.txt (한 줄에 하나: ecid=0x.., seed=0x.., installer=1, ram_mb=, cpus=, jit_mb=, seconds=, log=).\n");
                    base.mkdirs();
                } else {
                    java.util.Map<String, String> cfg = new java.util.HashMap<>();
                    File cf = new File(base, "config.txt");
                    if (cf.exists()) {
                        try (java.io.BufferedReader br = new java.io.BufferedReader(new java.io.FileReader(cf))) {
                            String line;
                            while ((line = br.readLine()) != null) {
                                int eq = line.indexOf('=');
                                if (eq > 0 && !line.trim().startsWith("#")) cfg.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
                            }
                        }
                    }
                    java.util.List<String> cmd = new java.util.ArrayList<>();
                    cmd.add(exe.getAbsolutePath());
                    cmd.add("--rom"); cmd.add(rom.getAbsolutePath());
                    cmd.add("--disk"); cmd.add(disk.getAbsolutePath());
                    cmd.add("--aux"); cmd.add(aux.getAbsolutePath());
                    cmd.add("--make-blank");
                    if ("1".equals(cfg.get("installer"))) cmd.add("--installer");
                    String[][] map = {{"ecid", "--ecid"}, {"seed", "--seed"}, {"ram_mb", "--ram-mb"}, {"cpus", "--cpus"},
                            {"jit_mb", "--jit-mb"}, {"seconds", "--seconds"}, {"log", "--log"}, {"aux_offset", "--aux-offset"}};
                    for (String[] m : map) if (cfg.containsKey(m[0])) { cmd.add(m[1]); cmd.add(cfg.get(m[0])); }
                    File snap = new File(getExternalFilesDir(null), "vm/snapshot.bmp");
                    cmd.add("--snapshot"); cmd.add(snap.getAbsolutePath());
                    acc.append("명령: ").append(android.text.TextUtils.join(" ", cmd)).append('\n');
                    show(acc.toString());
                    ProcessBuilder pb = new ProcessBuilder(cmd).directory(getFilesDir()).redirectErrorStream(true);
                    pb.environment().put("TMPDIR", getCacheDir().getAbsolutePath());
                    Process p = pb.start();
                    StringBuilder text = new StringBuilder();
                    Thread reader = new Thread(() -> {
                        try (java.io.BufferedReader br = new java.io.BufferedReader(new java.io.InputStreamReader(p.getInputStream()))) {
                            String line;
                            while ((line = br.readLine()) != null) {
                                synchronized (text) {
                                    text.append(line).append('\n');
                                    show(acc + text.toString());
                                }
                            }
                        } catch (IOException ignored) {
                        }
                    });
                    reader.start();
                    double secs = 30;
                    try { if (cfg.containsKey("seconds")) secs = Double.parseDouble(cfg.get("seconds")); } catch (NumberFormatException ignored) { }
                    boolean done = p.waitFor((long) secs + 60, java.util.concurrent.TimeUnit.SECONDS);
                    if (!done) p.destroyForcibly();
                    reader.join(2000);
                    synchronized (text) {
                        acc.append(text);
                    }
                    if (!done) acc.append("vmrun이 제한 시간 안에 끝나지 않아 강제 종료했습니다.\n");
                    else acc.append("vmrun 종료 코드: ").append(p.exitValue()).append(p.exitValue() > 128 ? " (128 초과: 시그널 종료일 수 있음, 확정 아님)" : "").append('\n');
                    acc.append("이 출력은 게스트가 부팅했다는 증거가 아닙니다. pc/el/esr 줄과 종료 사유를 확인하세요.\n");
                }
            } catch (Exception e) {
                acc.append("실행 오류: ").append(e).append('\n');
            }
            show(acc.toString());
            running = false;
            setButtons(true);
        }).start();
    }

    private void copyResult() {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        cm.setPrimaryClip(ClipData.newPlainText("henge-jit-probe", output.getText()));
        Toast.makeText(this, "복사했습니다", Toast.LENGTH_SHORT).show();
    }
}
