package dev.muns.hengeprobe;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Typeface;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.Build;
import android.os.Bundle;
import android.os.StatFs;
import android.system.Os;
import android.system.OsConstants;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
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
    private Button installerButton;
    private File crashFile;
    private File progressFile;
    private volatile boolean running;
    private volatile IpswDownload ipswDl;
    private volatile boolean dlDialogOpen;   // the confirmation dialog is up (or its status read is running): no second dialog, no delete meanwhile
    private volatile boolean dlWatching;     // watching a download that another process (earlier tap, other window) is already running
    private Button dlStopButton;
    private ScrollView pageScroll;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        crashFile = new File(getFilesDir(), "last_crash.txt");
        progressFile = new File(getFilesDir(), "progress.txt");

        int pad = (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 16, getResources().getDisplayMetrics());
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad * 2, pad, pad);

        // Finished tests (also the ROM boot, DFU, restore-tool, TLS, BuildManifest and TSS probes) live in one section that starts folded; the IPSW download buttons stay on top.
        final LinearLayout done = new LinearLayout(this);
        done.setOrientation(LinearLayout.VERTICAL);
        done.setVisibility(View.GONE);

        runButton = new Button(this);
        runButton.setText("JIT 크기 시험 실행 (1~768 MiB)");
        runButton.setOnClickListener(v -> runNow());
        done.addView(runButton);

        mtButton = new Button(this);
        mtButton.setText("다중 스레드 패치 시험 (1~4단계, 약 20초)");
        mtButton.setOnClickListener(v -> runMtNow(false));
        done.addView(mtButton);

        longButton = new Button(this);
        longButton.setText("장시간 반복 (60초)");
        longButton.setOnClickListener(v -> runMtNow(true));
        done.addView(longButton);

        memButton = new Button(this);
        memButton.setText("게스트 메모리 시험 (2~8 GiB 예약, 일부 페이지)");
        memButton.setOnClickListener(v -> runMemNow(0));
        done.addView(memButton);

        denseButton = new Button(this);
        denseButton.setText("단계적 사용 시험 (종류별 1 GiB까지 실제 사용)");
        denseButton.setOnClickListener(v -> runMemNow(1));
        done.addView(denseButton);

        reclaimButton = new Button(this);
        reclaimButton.setText("메모리 반납 시험 (balloon 대체 호출)");
        reclaimButton.setOnClickListener(v -> runMemNow(2));
        done.addView(reclaimButton);

        engineButton = new Button(this);
        engineButton.setText("엔진 시험 실행 (mmu/jit/mem/exc/devices/vm)");
        engineButton.setOnClickListener(v -> runEngineNow());
        done.addView(engineButton);

        bigVmButton = new Button(this);
        bigVmButton.setText("VM 대용량 시험 (RAM 2048MB, JIT 768MB)");
        bigVmButton.setOnClickListener(v -> runBigVmNow());
        done.addView(bigVmButton);

        vmrunButton = new Button(this);
        vmrunButton.setText("ROM 일반 부팅 시험 (vmrun, 30초)");
        vmrunButton.setOnClickListener(v -> runVmrun(false));
        done.addView(vmrunButton);

        installerButton = new Button(this);
        installerButton.setText("ROM 설치기(DFU) 부팅 시험 (VM 고유 ECID, 30초)");
        installerButton.setOnClickListener(v -> runVmrun(true));
        done.addView(installerButton);

        final Button irecvButton = new Button(this);
        irecvButton.setText("복원 라이브러리 USB 연결 시험 (libirecovery로 장치 열기, 30초)");
        irecvButton.setOnClickListener(v -> runVmrun(true, true));
        done.addView(irecvButton);
        final TextView irecvNote = new TextView(this);
        irecvNote.setText("위 버튼: 설치기(DFU)로 부팅한 뒤 복원 도구가 쓰는 libirecovery로 가상 USB 장치를 열고 닫기만 합니다. "
                + "USB 구성(SET_CONFIGURATION 1)을 선택하므로 장치 상태가 바뀔 수 있습니다. 펌웨어 전송·Apple 서명 요청·IPSW 다운로드는 하지 않습니다.");
        irecvNote.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        done.addView(irecvNote);

        final Button dnloadButton = new Button(this);
        dnloadButton.setText("DFU 더미 DNLOAD 시험 A0~A2 (16바이트 1회, 실제 ROM으로 전송)");
        dnloadButton.setOnClickListener(v -> new android.app.AlertDialog.Builder(this)
                .setTitle("실제 ROM에 DNLOAD를 보냅니다")
                .setMessage("설치기(DFU)로 부팅한 가상 장치에 USB 요청 6개를 보냅니다: GET_CONFIGURATION, 필요하면 SET_CONFIGURATION(1), "
                        + "GET_STATE, 0으로 채운 16바이트 DNLOAD(블록 0) 1회, GET_STATE. GET_STATUS·ABORT·길이 0 DNLOAD·두 번째 블록은 보내지 않고, "
                        + "예상 밖의 응답이 하나라도 나오면 즉시 멈춥니다. Apple 서명 요청·IPSW 다운로드·복원은 하지 않습니다. 실행할까요?")
                .setNegativeButton("취소", null)
                .setPositiveButton("실행", (d, w) -> runVmrun(true, false, true))
                .show());
        done.addView(dnloadButton);
        final TextView dnloadNote = new TextView(this);
        dnloadNote.setText("위 버튼: 결과는 '요청과 응답' 기록이며 ROM이 그 바이트를 펌웨어로 받아들였다는 뜻이 아닙니다. 누를 때마다 새 빈 디스크로 다시 부팅합니다.");
        dnloadNote.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        done.addView(dnloadNote);

        final Button a3Button = new Button(this);
        a3Button.setText("DFU 시험 A3: A0~A2 재현 + GET_STATUS 1회 (실제 ROM으로 전송)");
        a3Button.setOnClickListener(v -> new android.app.AlertDialog.Builder(this)
                .setTitle("실제 ROM에 DNLOAD와 GET_STATUS를 보냅니다")
                .setMessage("A0~A2(리셋, 구성, GET_STATE, 0으로 채운 16바이트 DNLOAD 블록 0, GET_STATE)를 앞선 시험과 똑같이 재현합니다. "
                        + "A2가 상태 5일 때만 GET_STATUS를 정확히 1회 보내고 그 뒤에는 아무것도 보내지 않습니다. GET_STATUS는 장치 상태를 앞으로 진행시킬 수 있습니다. "
                        + "ABORT·두 번째 GET_STATUS·길이 0 DNLOAD·두 번째 블록·Apple 서명 요청·IPSW 다운로드·복원은 하지 않습니다. 실행할까요?")
                .setNegativeButton("취소", null)
                .setPositiveButton("실행", (d, w) -> runVmrun(true, false, true, true))
                .show());
        done.addView(a3Button);

        final Button a4Button = new Button(this);
        a4Button.setText("DFU 시험 A4: A0~A3 재현 + ABORT 1회 + GET_STATE 1회 (실제 ROM으로 전송)");
        a4Button.setOnClickListener(v -> new android.app.AlertDialog.Builder(this)
                .setTitle("실제 ROM에 ABORT를 보냅니다")
                .setMessage("A0~A3(리셋, 구성, GET_STATE, 0으로 채운 16바이트 DNLOAD, GET_STATE, GET_STATUS 1회)를 앞선 시험과 똑같이 재현합니다. "
                        + "GET_STATUS가 오류 없음(bStatus 0)과 상태 5를 돌려줄 때만 DFU ABORT를 정확히 1회 보내고, 이어서 GET_STATE를 1회 보낸 뒤 아무것도 보내지 않습니다. "
                        + "CLR_STATUS·재시도·두 번째 ABORT·길이 0 DNLOAD·두 번째 블록·Apple 서명 요청·IPSW 다운로드·복원은 하지 않습니다. 실행할까요?")
                .setNegativeButton("취소", null)
                .setPositiveButton("실행", (d, w) -> runVmrun(true, false, true, 4))
                .show());
        done.addView(a4Button);

        final Button selftestButton = new Button(this);
        selftestButton.setText("복원 도구 자체 시험 (앱 안에서 적재·스레드·loopback, 잘못된 IPSW 3종)");
        selftestButton.setOnClickListener(v -> runRestoreSelftest());
        done.addView(selftestButton);
        final TextView selftestNote = new TextView(this);
        selftestNote.setText("위 버튼: 복원 도구 전체(usbmuxd·libirecovery·libtatsu·idevicerestore)를 이 앱 안에서 시작해 가짜 IPSW 3개로 깔끔히 실패하는지, "
                + "usbmuxd가 loopback에서 ListDevices에 답하는지 봅니다. 네트워크·장치·Apple 서버를 쓰지 않습니다. 실제 복원이 되는지와는 별개입니다.");
        selftestNote.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        done.addView(selftestNote);

        final Button tlsButton = new Button(this);
        tlsButton.setText("HTTPS·CA 저장소 확인 (공개 사이트)");
        tlsButton.setOnClickListener(v -> runTlsCheck(false));
        done.addView(tlsButton);

        final Button appleButton = new Button(this);
        appleButton.setText("Apple 서명 서버 TLS 확인 (GET 한 번)");
        appleButton.setOnClickListener(v -> runTlsCheck(true));
        done.addView(appleButton);

        final Button bmButton = new Button(this);
        bmButton.setText("BuildManifest 부분 조회 (Apple CDN, Range 요청만)");
        bmButton.setOnClickListener(v -> runBuildManifestProbe());
        done.addView(bmButton);
        final TextView bmNote = new TextView(this);
        bmNote.setText("macOS 15.6.1 IPSW(약 16 GiB)에서 BuildManifest.plist만 Range 요청으로 읽습니다(최대 24회·4 MiB, 강한 ETag 필수, 파일 저장 없음). "
                + "전체 다운로드·TSS 요청·복원은 하지 않습니다. 결과(요청 범위·응답 코드·칩/보드 일치)가 이 화면에 나옵니다.");
        bmNote.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        done.addView(bmNote);

        final Button tssButton = new Button(this);
        tssButton.setText("TSS 서명 가능성 확인 (Apple 서버로 1회 전송)");
        tssButton.setOnClickListener(v -> confirmTssProbe());
        done.addView(tssButton);
        final TextView tssNote = new TextView(this);
        tssNote.setText("가상 Mac을 DFU까지 부팅해 ROM이 알려주는 ECID·APNonce·SEPNonce를 읽고, macOS 15.6.1 Erase Install 구성으로 만든 서명 요청을 Apple에 정확히 1회 보냅니다. "
                + "누르면 먼저 확인 창이 나옵니다. 받은 서명 값은 저장·표시하지 않습니다. 복원·IPSW 전체 다운로드·펌웨어 전송은 하지 않습니다.");
        tssNote.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        done.addView(tssNote);

        final Button dlButton = new Button(this);
        dlButton.setText("IPSW 다운로드 (전체 15.66 GiB, 이어받기)");
        dlButton.setOnClickListener(v -> confirmIpswDownload());
        root.addView(dlButton);
        dlStopButton = new Button(this);
        dlStopButton.setText("다운로드 중단 (나중에 이어받기 가능)");
        dlStopButton.setEnabled(false);
        dlStopButton.setOnClickListener(v -> { IpswDownload d = ipswDl; if (d != null) { d.cancel(); Toast.makeText(this, "중단 요청함", Toast.LENGTH_SHORT).show(); } });
        root.addView(dlStopButton);
        final Button dlStatusButton = new Button(this);
        dlStatusButton.setText("IPSW 다운로드 상태 보기");
        dlStatusButton.setOnClickListener(v -> showIpswStatus());
        root.addView(dlStatusButton);
        final Button dlDeleteButton = new Button(this);
        dlDeleteButton.setText("IPSW 받던 파일 삭제 (처음부터 다시)");
        dlDeleteButton.setOnClickListener(v -> confirmIpswDelete());
        root.addView(dlDeleteButton);
        final TextView dlNote = new TextView(this);
        dlNote.setText("macOS 15.6.1 IPSW(16,814,137,790바이트, 약 15.66 GiB)를 앱 내부 저장소에 끝까지 받습니다. 이미 받은 부분(Range·강한 ETag·If-Range)에서 이어받고, 끊기면 자동으로 다시 연결합니다. "
                + "받는 동안 화면이 꺼지지 않으니 앱을 앞에 두세요(포그라운드 서비스는 아직 없습니다). 전체를 받고 SHA-256이 기대값과 같을 때만 최종 파일로 확정합니다. "
                + "누르면 먼저 확인 창이 나옵니다. 서명(TSS)·복원·VM 실행은 하지 않습니다.");
        dlNote.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        root.addView(dlNote);

        final Button toggle = new Button(this);
        toggle.setText("▶ 완료된 시험 펼치기 (끝난 시험 전부, 다시 쓰지 않음)");
        toggle.setOnClickListener(v -> {
            boolean open = done.getVisibility() != View.VISIBLE;
            done.setVisibility(open ? View.VISIBLE : View.GONE);
            toggle.setText(open ? "▼ 완료된 시험 접기" : "▶ 완료된 시험 펼치기 (끝난 시험 전부, 다시 쓰지 않음)");
        });
        root.addView(toggle);
        root.addView(done);

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
        // The whole page scrolls (buttons and result together). Before, only the result box scrolled, and with this many buttons it was squeezed to nothing and the buttons
        // below the screen could not be reached.
        root.addView(output, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        // the same copy button under the result (the one above scrolls out of sight when a run brings the result into view), and a way back to the top
        Button copyBottom = new Button(this);
        copyBottom.setText("결과 복사");
        copyBottom.setOnClickListener(v -> copyResult());
        root.addView(copyBottom);
        Button toTop = new Button(this);
        toTop.setText("맨 위로");
        toTop.setOnClickListener(v -> pageScroll.smoothScrollTo(0, 0));
        root.addView(toTop);
        pageScroll = new ScrollView(this);
        pageScroll.setFillViewport(true);
        pageScroll.addView(root, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(pageScroll);

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
        sb.append("\n위쪽의 ROM 부팅 시험 버튼을 쓰세요. 이전에 끝난 시험은 \"완료된 시험 펼치기\" 안에 있습니다.\n");
        output.setText(sb.toString());
    }

    private String deviceInfo() {
        long page = -1;
        try {
            page = Os.sysconf(OsConstants._SC_PAGESIZE);
        } catch (Exception ignored) {
        }
        long build = 0;
        String sha = "?";
        try {
            android.content.pm.PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
            build = pi.getLongVersionCode();
            sha = pi.versionName;
        } catch (Exception ignored) {
        }
        return "빌드 번호: " + build + " (CI 실행 번호; 클수록 최신, 커밋 " + sha + ")\n기기: " + Build.MANUFACTURER + " " + Build.MODEL + "\n"
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
            // a run starts: bring the result box into view once (the user can scroll away afterwards)
            if (!enabled && pageScroll != null && output != null) pageScroll.post(() -> pageScroll.smoothScrollTo(0, output.getTop()));
            runButton.setEnabled(enabled);
            mtButton.setEnabled(enabled);
            longButton.setEnabled(enabled);
            memButton.setEnabled(enabled);
            denseButton.setEnabled(enabled);
            reclaimButton.setEnabled(enabled);
            engineButton.setEnabled(enabled);
            bigVmButton.setEnabled(enabled);
            vmrunButton.setEnabled(enabled);
            installerButton.setEnabled(enabled);
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

    private static final String ROM_URL = "https://updates.cdn-apple.com/2024SummerFCS/fullrestores/062-52859/932E0A8F-6644-4759-82DA-F8FA8DEA806A/UniversalMac_14.6.1_23G93_Restore.ipsw";
    private static final long ROM_BLOCK_OFFSET = 5349700747L;
    private static final int ROM_BLOCK_LENGTH = 859848, ROM_LENGTH = 214904;
    private static final String ROM_SHA256 = "fd4973e42e609751830df6f3dd2d2b2ad2309fe38f0ca0f5aba02dcbefe45f91";

    /** True if the file is exactly the known AVPBooter ROM (size and SHA-256). */
    private static boolean isGenuineRom(File f) {
        if (!f.isFile() || f.length() != ROM_LENGTH) return false;
        try (FileInputStream in = new FileInputStream(f)) {
            java.security.MessageDigest md = java.security.MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) md.update(buf, 0, n);
            StringBuilder hex = new StringBuilder();
            for (byte b : md.digest()) hex.append(String.format("%02x", b));
            return hex.toString().equals(ROM_SHA256);
        } catch (Exception e) {
            return false;
        }
    }

    /**
     * The AVPBooter ROM in the app's private folder: the cached one if its size and SHA-256 are right, else downloaded from
     * Apple (one HTTP Range request for the 859,848 bytes that hold it, as Henge's iOS app does) and unpacked by
     * libromextract.so. Each failure has its own message in acc. Returns null on failure.
     */
    private File ensureRom(StringBuilder acc) {
        File dir = new File(getFilesDir(), "vm");
        dir.mkdirs();
        File rom = new File(dir, "AVPBooter.vmapple2.bin");
        if (isGenuineRom(rom)) {
            acc.append("ROM: 저장된 파일의 크기와 SHA-256이 맞습니다 (다시 받지 않음).\n");
            return rom;
        }
        if (rom.exists()) {
            acc.append("ROM: 저장된 파일이 맞지 않아 지우고 다시 받습니다.\n");
            rom.delete();
        }
        File block = new File(getCacheDir(), "rom_block.bin");
        String failure = "응답 없음";
        boolean got = false;
        for (int attempt = 1; attempt <= 3 && !got; attempt++) {
            java.net.HttpURLConnection c = null;
            try {
                c = (java.net.HttpURLConnection) new java.net.URL(ROM_URL).openConnection();
                c.setConnectTimeout(20000);
                c.setReadTimeout(30000);
                c.setRequestProperty("Range", "bytes=" + ROM_BLOCK_OFFSET + "-" + (ROM_BLOCK_OFFSET + ROM_BLOCK_LENGTH - 1));
                int status = c.getResponseCode();
                if (status != 206) {
                    failure = "HTTP 상태 " + status + " (206 아님: Range 요청이 받아들여지지 않음)";
                } else {
                    long total = 0;
                    try (java.io.InputStream in = c.getInputStream(); java.io.FileOutputStream out = new java.io.FileOutputStream(block)) {
                        byte[] buf = new byte[65536];
                        int n;
                        while ((n = in.read(buf)) > 0) {
                            out.write(buf, 0, n);
                            total += n;
                        }
                    }
                    if (total != ROM_BLOCK_LENGTH) failure = "받은 바이트 " + total + " (기대 " + ROM_BLOCK_LENGTH + ")";
                    else got = true;
                }
            } catch (SecurityException e) {
                acc.append("ROM 다운로드 중단: 앱에 네트워크 권한(INTERNET)이 없습니다 (재시도하지 않음): ").append(e).append('\n');
                return null;
            } catch (Exception e) {
                failure = "네트워크 오류: " + e;
            } finally {
                if (c != null) c.disconnect();
            }
            if (!got) {
                acc.append("ROM 다운로드 ").append(attempt).append("/3 실패: ").append(failure).append('\n');
                try { Thread.sleep(attempt * 1000L); } catch (InterruptedException ignored) { }
            }
        }
        if (!got) return null;
        acc.append("ROM: 859,848바이트 받음 (HTTP 206).\n");
        try {
            File exe = new File(getApplicationInfo().nativeLibraryDir, "libromextract.so");
            if (!exe.exists()) {
                acc.append("ROM 추출기(libromextract.so)가 이 APK에 없습니다.\n");
                return null;
            }
            Process p = new ProcessBuilder(exe.getAbsolutePath(), block.getAbsolutePath(), rom.getAbsolutePath()).redirectErrorStream(true).start();
            String out = new String(readAll(p.getInputStream()), StandardCharsets.UTF_8);
            int rc = p.waitFor();
            acc.append(out);
            if (rc != 0 || !isGenuineRom(rom)) {
                String why;
                switch (rc) {
                    case 3: why = "받은 조각의 크기가 다릅니다"; break;
                    case 4: why = "LZFSE 해제 실패"; break;
                    case 5: why = "내부 리소스 포크 형식 오류"; break;
                    case 6: why = "LZVN 해제 실패"; break;
                    case 7: why = "크기 또는 SHA-256 불일치 (받은 데이터가 예상한 ROM이 아님)"; break;
                    default: why = "추출기 종료 코드 " + rc;
                }
                acc.append("ROM 추출 실패: ").append(why).append('\n');
                rom.delete();
                return null;
            }
            acc.append("ROM: 추출 성공, 크기와 SHA-256 확인됨.\n");
            return rom;
        } catch (Exception e) {
            acc.append("ROM 추출 중 오류: ").append(e).append('\n');
            return null;
        } finally {
            block.delete();
        }
    }

    private static byte[] readAll(java.io.InputStream in) throws IOException {
        java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) bo.write(buf, 0, n);
        return bo.toByteArray();
    }

    private void runVmrunNow() {
        runVmrun(false);
    }

    /** installer=true boots the ROM in DFU (installer) mode. Both modes use new blank disk/aux files each time and log PSCI calls. */
    private void runVmrun(final boolean installer) {
        runVmrun(installer, false);
    }

    /** irecv=true (needs installer): the program with libirecovery in it opens the virtual USB device through the library (--irecv-probe instead of --usb-probe). */
    private void runVmrun(final boolean installer, final boolean irecv) {
        runVmrun(installer, irecv, false);
    }

    /** dnload=true (needs installer): --dnload-probe, stage A0-A2 of the DFU download test (host/dnload_probe.h), run by the same program as irecv. */
    private void runVmrun(final boolean installer, final boolean irecvIn, final boolean dnload) {
        runVmrun(installer, irecvIn, dnload, 2);
    }

    /** a3=true (with dnload): --dnload-probe-a3, A0-A2 and then one GET_STATUS. */
    private void runVmrun(final boolean installer, final boolean irecvIn, final boolean dnload, final boolean a3) {
        runVmrun(installer, irecvIn, dnload, a3 ? 3 : 2);
    }

    /** stage (with dnload): 2 = --dnload-probe (A0-A2), 3 = --dnload-probe-a3 (+ one GET_STATUS), 4 = --dnload-probe-a4 (+ one ABORT and one GET_STATE). */
    private void runVmrun(final boolean installer, final boolean irecvIn, final boolean dnload, final int stage) {
        final boolean tss = tssMode;
        tssMode = false;
        final boolean irecv = irecvIn;
        if (running) return;
        running = true;
        setButtons(false);
        final String libDir = getApplicationInfo().nativeLibraryDir;
        new Thread(() -> {
            StringBuilder acc = new StringBuilder();
            try {
                File base = new File(getExternalFilesDir(null), "vm");
                File rom = new File(base, "rom.bin"); // a ROM placed here is used as it is; otherwise the app fetches and checks its own
                // disk and aux: the ones in that folder if present, else blank ones in the app's private folder (made by vmrun)
                File priv = new File(getFilesDir(), "vm");
                priv.mkdirs();
                File disk = new File(base, "disk.img").exists() ? new File(base, "disk.img") : new File(priv, "blank_disk.img");
                File aux = new File(base, "aux.img").exists() ? new File(base, "aux.img") : new File(priv, "blank_aux.img");
                acc.append("폴더: ").append(base).append('\n');
                File exe = new File(libDir, tss ? "libvmruntss.so" : irecv || dnload ? "libvmrunirecv.so" : "libvmrun.so");
                if (!exe.exists()) acc.append(tss ? "TSS 시험용 vmrun이 이 APK에 없습니다(빌드 실패). 아무것도 전송하지 않았습니다.\n" : irecv || dnload ? "libirecovery가 들어간 vmrun이 이 APK에 없습니다(빌드 실패).\n" : "vmrun 실행 파일이 이 APK에 없습니다.\n");
                else if (!rom.exists() && (rom = ensureRom(acc)) == null) {
                    acc.append("ROM을 준비하지 못해 실행하지 않았습니다. 위 메시지가 사유입니다.\n");
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
                    cmd.add("--recreate-blank");
                    cmd.add("--psci-log");
                    if (installer) cmd.add(tss ? "--tss-probe" : dnload ? (stage >= 4 ? "--dnload-probe-a4" : stage == 3 ? "--dnload-probe-a3" : "--dnload-probe") : irecv ? "--irecv-probe" : "--usb-probe");
                    if (installer || "1".equals(cfg.get("installer"))) cmd.add("--installer");
                    acc.append(installer ? "모드: 설치기(DFU) 부팅\n" : "모드: 일반 부팅\n");
                    Long vmEcid = null;
                    if (installer && !cfg.containsKey("ecid")) {
                        VmEcid.Result er = VmEcid.loadOrCreate(priv);
                        switch (er.status) {
                            case CREATED: acc.append("이 VM의 ECID를 새로 만들어 ").append(er.detail).append("에 저장했습니다.\n"); break;
                            case LOADED: acc.append("기존 ECID를 ").append(er.detail).append("에서 읽음").append(tss ? " (값은 표시하지 않음)" : ": " + String.format("0x%x", er.value)).append('\n'); break;
                            default:
                                acc.append("ECID를 쓸 수 없어 멈춥니다(파일은 바꾸지 않았습니다): ").append(er.detail)
                                   .append(er.status == VmEcid.Status.CORRUPT ? " — 내용을 확인하고, 버리려면 직접 지운 뒤 다시 누르세요.\n" : "\n");
                                show(acc.toString()); running = false; setButtons(true); return;
                        }
                        vmEcid = er.value;
                        cfg.put("ecid", String.format("0x%x", vmEcid));
                    }
                    String[][] map = {{"ecid", "--ecid"}, {"seed", "--seed"}, {"ram_mb", "--ram-mb"}, {"cpus", "--cpus"},
                            {"jit_mb", "--jit-mb"}, {"seconds", "--seconds"}, {"log", "--log"}, {"aux_offset", "--aux-offset"}};
                    for (String[] m : map) if (cfg.containsKey(m[0])) { cmd.add(m[1]); cmd.add(cfg.get(m[0])); }
                    File snap = new File(getExternalFilesDir(null), "vm/snapshot.bmp");
                    cmd.add("--snapshot"); cmd.add(snap.getAbsolutePath());
                    java.util.List<String> shown = new java.util.ArrayList<>(cmd);
                    if (tss) for (int k = 0; k + 1 < shown.size(); k++) if (shown.get(k).equals("--ecid")) shown.set(k + 1, "(표시 안 함)");
                    acc.append("명령: ").append(android.text.TextUtils.join(" ", shown)).append('\n');
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
                    if (installer && !dnload && !tss) { // the dnload probe does not read the DFU string, so there is nothing to compare
                        String want = cfg.containsKey("ecid") ? cfg.get("ecid") : "0";
                        try {
                            String w = String.format("%016X", Long.decode(want));
                            java.util.regex.Matcher mm = java.util.regex.Pattern.compile("ECID:([0-9A-Fa-f]{16})").matcher(text);
                            String got = mm.find() ? mm.group(1).toUpperCase() : null;
                            acc.append("DFU 문자열 ECID 확인: 설정 ").append(w).append(" / DFU ").append(got == null ? "(읽지 못함)" : got)
                               .append(w.equals(got) ? " → 일치 (ECID 전달만 확인, Apple 서명 수용은 별개)\n" : " → 불일치 또는 미확인\n");
                        } catch (NumberFormatException ex) {
                            acc.append("config.txt의 ecid 값을 해석하지 못했습니다: ").append(want).append('\n');
                        }
                    }
                    if (dnload) {
                        StringBuilder sum = new StringBuilder();
                        for (String ln : text.toString().split("\n")) if (ln.startsWith("dnload probe:")) sum.append(ln).append('\n');
                        acc.append(sum.length() > 0 ? "\n— 요약 (dnload probe 줄 전체) —\n" + sum : "\n— dnload probe 줄이 없습니다: 시험이 시작되지 못했습니다 —\n");
                    }
                    if (irecv) {
                        String all = text.toString();
                        int a = all.indexOf("irecv probe: ===== result =====");
                        int b = all.indexOf("irecv probe: done");
                        if (a >= 0) {
                            int e = b >= a ? all.indexOf('\n', b) : -1;
                            acc.append("\n— 요약 —\n").append(all, a, e > 0 ? e : all.length()).append('\n');
                        } else {
                            acc.append("\n— 요약 없음: 시험이 결과 표까지 가지 못했습니다(위 irecv probe 줄 참고) —\n");
                        }
                    }
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

    /** Runs librestoreselftest.so (tests/test_restore_stack.c: the whole restore stack started the way the app starts it) three times, each with an IPSW that
     *  cannot be restored: a path that does not exist, 4 KB of random bytes, a zip without BuildManifest. Pass = exit 0, usbmuxd answered a ListDevices on the
     *  loopback socket, and the restore ended in a clean failure (RESTORE_RESULT=-1). No network, no device, no Apple server. */
    private void runRestoreSelftest() {
        if (running) return;
        running = true;
        setButtons(false);
        final String libDir = getApplicationInfo().nativeLibraryDir;
        new Thread(() -> {
            StringBuilder acc = new StringBuilder();
            try {
                File exe = new File(libDir, "librestoreselftest.so");
                if (!exe.exists()) {
                    acc.append("복원 도구 자체 시험 프로그램이 이 APK에 없습니다(빌드 실패).\n");
                } else {
                    File work = new File(getFilesDir(), "rs");
                    File w = new File(work, "w");
                    w.mkdirs();
                    File bad = new File(work, "bad.ipsw");
                    byte[] rnd = new byte[4096];
                    new java.security.SecureRandom().nextBytes(rnd);
                    try (java.io.FileOutputStream fo = new java.io.FileOutputStream(bad)) { fo.write(rnd); }
                    File nom = new File(work, "nomanifest.ipsw");
                    try (java.util.zip.ZipOutputStream zo = new java.util.zip.ZipOutputStream(new java.io.FileOutputStream(nom))) {
                        zo.putNextEntry(new java.util.zip.ZipEntry("readme.txt"));
                        zo.write("x".getBytes(StandardCharsets.UTF_8));
                        zo.closeEntry();
                    }
                    File[] inputs = {new File(work, "missing.ipsw"), bad, nom};
                    String[] names = {"없는 파일", "무작위 4KB", "BuildManifest 없는 zip"};
                    int passed = 0;
                    for (int i = 0; i < inputs.length; i++) {
                        ProcessBuilder pb = new ProcessBuilder(exe.getAbsolutePath(), inputs[i].getAbsolutePath(), w.getAbsolutePath()).directory(getFilesDir()).redirectErrorStream(true);
                        pb.environment().put("TMPDIR", getCacheDir().getAbsolutePath());
                        Process p = pb.start();
                        StringBuilder out = new StringBuilder();
                        Thread rd = new Thread(() -> {
                            try (java.io.BufferedReader br = new java.io.BufferedReader(new java.io.InputStreamReader(p.getInputStream()))) {
                                String line;
                                while ((line = br.readLine()) != null) synchronized (out) { out.append(line).append('\n'); }
                            } catch (IOException ignored) { }
                        });
                        rd.start();
                        boolean done = p.waitFor(90, java.util.concurrent.TimeUnit.SECONDS);
                        if (!done) p.destroyForcibly();
                        rd.join(2000);
                        String o;
                        synchronized (out) { o = out.toString(); }
                        int code = done ? p.exitValue() : -999;
                        boolean ok = done && code == 0 && o.contains("MUX_LISTENING=1") && o.contains("MUX_PROTOCOL_OK=1") && o.contains("RESTORE_RESULT=-1");
                        if (ok) passed++;
                        acc.append(ok ? "통과 " : "실패 ").append(names[i]).append(" (종료 코드 ").append(done ? String.valueOf(code) : "시간 초과").append(")\n");
                        for (String ln : o.split("\n")) if (ln.startsWith("MUX_") || ln.startsWith("RESTORE_RESULT") || ln.startsWith("hv_install_start")) acc.append("  ").append(ln).append('\n');
                        if (!ok) {
                            String[] lines = o.split("\n");
                            acc.append("  — 출력 끝부분 —\n");
                            for (int k = Math.max(0, lines.length - 15); k < lines.length; k++) acc.append("  ").append(lines[k]).append('\n');
                        }
                        show(acc.toString());
                    }
                    acc.append("\n결과: ").append(passed).append("/3 통과. 이것은 앱 안에서 복원 도구가 적재되고 스레드가 돌며 loopback usbmuxd가 응답하고 잘못된 IPSW에서 깔끔히 실패한다는 확인이며, 실제 복원 성공의 증거가 아닙니다.\n");
                }
            } catch (Exception e) {
                acc.append("실행 오류: ").append(e).append('\n');
            }
            show(acc.toString());
            running = false;
            setButtons(true);
        }).start();
    }

    /** Runs libcurltool.so (host/curl_mbedtls.c, the client that will serve the restore tools) against real servers and reports the
     *  three layers apart: CA store, TLS/certificate check, HTTP status. apple=true asks gs.apple.com once with a plain GET (no signing request). */
    private void runTlsCheck(final boolean apple) {
        if (running) return;
        running = true;
        setButtons(false);
        final String libDir = getApplicationInfo().nativeLibraryDir;
        new Thread(() -> {
            StringBuilder acc = new StringBuilder();
            acc.append("확인 중입니다… 단계마다 이 화면에 결과가 추가됩니다(최대 1~2분).\n");
            show(acc.toString());
            File exe = new File(libDir, "libcurltool.so");
            if (!exe.exists()) acc.append("libcurltool.so가 이 APK에 없습니다.\n");
            else {
                acc.append("1) CA 저장소\n").append(runTool(exe, "--ca"));
                show(acc.toString() + "\n(접속 확인 중…)\n");
                String[] urls = apple ? new String[] {"https://gs.apple.com/TSS/controller?action=2"}
                                      : new String[] {"https://example.com/", "https://www.apple.com/"};
                for (String u : urls) {
                    String o = runTool(exe, u, "--discard");
                    acc.append("\n2) ").append(u).append('\n').append(o);
                    show(acc.toString());
                    java.util.regex.Matcher m = java.util.regex.Pattern.compile("RC=(\\d+) HTTP=(\\d+) BYTES=(\\d+) ERR=(.*)").matcher(o);
                    boolean tlsDone = o.contains("TLS_VERIFIED=1");
                    if (m.find()) {
                        int rc = Integer.parseInt(m.group(1));
                        String tls = rc == 0 || rc == 56 || rc == 23 || rc == 52 ? "TLS/인증서 검증 통과"
                                : rc == 60 ? "인증서 검증 실패(거부됨)" : rc == 35 ? "TLS 핸드셰이크 실패" : rc == 77 ? "신뢰할 CA를 불러오지 못해 연결 안 함"
                                : rc == 6 || rc == 7 || rc == 28 ? "네트워크 연결 단계 실패(TLS 판정 불가)" : "기타 오류 " + rc;
                        if (tlsDone) tls = "TLS 핸드셰이크 완료 + 인증서·호스트 검증 통과" + (rc == 0 ? "" : " (이후 전송 오류 " + rc + ")");
                        acc.append("   → TLS: ").append(tls).append(" / HTTP: ").append(rc == 0 ? m.group(2) : "(응답 없음)").append('\n');
                    }
                }
                acc.append("\nHTTP 4xx/5xx여도 TLS가 통과면 연결·인증서 쪽은 정상입니다. 이 시험은 복원이나 서명 요청 성공이 아닙니다.\n");
            }
            show(acc.toString());
            running = false;
            setButtons(true);
        }).start();
    }

    private boolean tssMode;

    /** The confirmation before anything leaves the phone: says exactly what is sent, once, and where. Cancel sends nothing. */
    private void confirmTssProbe() {
        if (running) return;
        new AlertDialog.Builder(this)
            .setTitle("Apple 서명 서버로 1회 전송합니다")
            .setMessage("대상: macOS Sequoia 15.6.1 (24G90), 가상 Mac(칩 0xFE00·보드 0x20)의 Customer Erase Install 구성\n\n"
                + "먼저 IPSW에서 BuildManifest만 Range 요청으로 읽습니다(최대 24회·4 MiB, 파일 저장 없음).\n\n"
                + "Apple(gs.apple.com, HTTPS·인증서 검증)로 나가는 정보:\n"
                + "• 이 가상 Mac의 ECID\n• ROM이 알려준 APNonce·SEPNonce\n• 해당 빌드 구성의 구성요소 해시\n• 이 폰의 IP 주소(접속 자체에 따름)\n\n"
                + "전송은 정확히 1회이며 자동 재시도가 없습니다. 받은 서명 값과 ECID·nonce는 저장하거나 화면에 표시하지 않습니다. "
                + "결과가 '서명 발급'이어도 복원 성공을 뜻하지 않습니다(복원에는 요청이 더 필요합니다).\n\n"
                + "IPSW 전체 다운로드, 펌웨어 전송, 복원은 하지 않습니다.")
            .setPositiveButton("전송", (d, w) -> { tssMode = true; runVmrun(true, false, false, 2); })
            .setNegativeButton("취소", null)
            .show();
    }

    // ---- IPSW download (first stage: up to 256 MiB per run; the work is done by libipswdl.so, see IpswDownload) ----

    private File ipswDir() {
        File d = new File(getFilesDir(), "vm");
        d.mkdirs();
        return d;
    }

    /** WIFI = unmetered Wi-Fi; CELLULAR; OTHER = something else or metered; NONE; UNKNOWN = could not be determined. */
    private String networkKind() {
        try {
            ConnectivityManager cm = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
            Network n = cm.getActiveNetwork();
            NetworkCapabilities c = n == null ? null : cm.getNetworkCapabilities(n);
            if (c == null) return "NONE";
            if (c.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) && c.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED)) return "WIFI";
            if (c.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)) return "CELLULAR";
            return "OTHER";
        } catch (Throwable t) {
            return "UNKNOWN";
        }
    }

    private static String gib(long b) {
        return String.format(java.util.Locale.US, "%.2f GiB", b / 1073741824.0);
    }

    private static long field(String s, String key) {
        int i = s.indexOf(key + "=");
        if (i < 0) return Long.MIN_VALUE;
        int j = i + key.length() + 1, k = j;
        while (k < s.length() && (Character.isDigit(s.charAt(k)) || (k == j && s.charAt(k) == '-'))) k++;
        try { return Long.parseLong(s.substring(j, k)); } catch (NumberFormatException e) { return Long.MIN_VALUE; }
    }

    /** Reads the state first (no network), then asks. The program re-checks everything itself. */
    private void confirmIpswDownload() {
        if (running || dlDialogOpen || dlWatching) return;
        File exe = new File(getApplicationInfo().nativeLibraryDir, "libipswdl.so");
        if (!exe.exists()) { show("libipswdl.so가 이 APK에 없습니다(빌드 실패). 아무것도 받지 않았습니다.\n"); return; }
        dlDialogOpen = true;   // stays set from this tap until the dialog is gone: a second tap cannot open a second dialog, and the delete button waits
        new Thread(() -> {
            IpswDownload.Result st = new IpswDownload(exe, ipswDir()).status();
            final long committed = field(st.detail, "committed"), part = field(st.detail, "part"), fin = field(st.detail, "final");
            final boolean metaOk = st.detail.contains("meta=ok"), metaNone = st.detail.contains("meta=none");
            if (st.detail.contains("lock=held")) {   // a downloader of this file is already alive: do not start another, show what it is doing
                dlDialogOpen = false;
                watchRunningIpsw(exe);
                return;
            }
            if (fin > 0) { dlDialogOpen = false; runOnUiThread(() -> runIpswDownload(exe, 0, true)); return; }   // a finished file: the program only verifies it, no network
            boolean fresh = metaNone && part <= 0;                                           // nothing on disk
            boolean consistent = metaOk && (part >= committed || committed == 0);            // the program cuts a longer .part, refuses a shorter one
            if (!fresh && !consistent) {
                dlDialogOpen = false;
                show("받던 파일의 상태를 확인하지 못해 시작하지 않았습니다(메타데이터 손상, .part 없음/짧음 등). 아무것도 보내지 않았습니다.\n" + st.detail
                        + "\n처음부터 받으려면 'IPSW 받던 파일 삭제'를 누르세요.\n");
                return;
            }
            final long have = fresh ? 0 : committed;
            runOnUiThread(() -> askIpswDownload(exe, have));
        }).start();
    }

    /**
     * A downloader process of this file is alive (started by an earlier tap, it outlives the screen). Nothing is started; every few seconds the state is read again
     * (no network) and shown: bytes, speed from the change, seconds since the metadata was written. It cannot stop that process from here (no way to reach it yet):
     * force-stopping the app does, and the download resumes later.
     */
    private void watchRunningIpsw(final File exe) {
        if (dlWatching) return;
        dlWatching = true;
        runOnUiThread(() -> getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
        final long t0 = System.currentTimeMillis();
        final String head = "이미 다른 곳에서 IPSW를 받고 있습니다. 새로 시작하지 않았습니다. (이 화면은 5초마다 상태만 읽습니다. 네트워크 요청은 하지 않습니다.)\n"
                + "이 화면에서는 그 다운로드를 멈출 수 없습니다. 멈추려면 설정 → 앱 → 이 앱 → 강제 종료를 누르세요(받은 부분은 남고 다시 이어받을 수 있습니다).\n\n";
        new Thread(() -> {
            long prev = -1, prevT = 0; double ema = 0;
            try {
                while (!isFinishing() && !isDestroyed()) {
                    IpswDownload.Result r = new IpswDownload(exe, ipswDir()).status();
                    long now = System.currentTimeMillis();
                    long com = field(r.detail, "committed"), age = field(r.detail, "meta_age"), fin = field(r.detail, "final");
                    boolean held = r.detail.contains("lock=held");
                    if (prev >= 0 && com > prev && now > prevT) {
                        double inst = (com - prev) / ((now - prevT) / 1000.0);
                        ema = ema == 0 ? inst : 0.7 * ema + 0.3 * inst;
                    }
                    if (com != prev) { prev = com; prevT = now; }
                    if (!held) {
                        show(fin > 0 ? "그 다운로드가 끝났습니다. 최종 파일이 있습니다(" + gib(fin) + "). 'IPSW 다운로드' 버튼을 누르면 SHA-256 검증만 다시 합니다.\n"
                                     : "그 다운로드 프로세스가 끝났습니다. 확정된 양: " + gib(Math.max(com, 0)) + " / 15.66 GiB. 'IPSW 다운로드' 버튼으로 이어받을 수 있습니다.\n" + r.detail + "\n");
                        return;
                    }
                    String eta = ema > 1 && com >= 0 ? hms((16814137790L - com) / ema) : "계산 중";
                    show(head + String.format(java.util.Locale.US, "확정된 양: %s / 15.66 GiB (%.2f%%)\n최근 속도(확정 단위 16 MiB로 계단식): %.1f MB/s   남은 시간: %s\n"
                            + "메타데이터 마지막 갱신: %d초 전 (몇 분째 그대로면 멈췄을 수 있습니다)\n지켜본 시간: %s\n\n%s\n",
                            gib(Math.max(com, 0)), 100.0 * Math.max(com, 0) / 16814137790L, ema / 1048576.0, eta, age, hms((now - t0) / 1000.0), r.detail));
                    try { Thread.sleep(5000); } catch (InterruptedException e) { return; }
                }
            } finally {
                dlWatching = false;
                runOnUiThread(() -> getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
            }
        }).start();
    }

    private void askIpswDownload(final File exe, final long have) {
        final long total = 16814137790L;
        long free;
        try { free = new StatFs(ipswDir().getAbsolutePath()).getAvailableBytes(); } catch (Throwable t) { free = -1; }
        String net = networkKind();
        boolean wifi = net.equals("WIFI");
        long remain = total - have;
        String netText = wifi ? "Wi-Fi(종량제 아님)입니다."
                : net.equals("CELLULAR") ? "모바일 데이터입니다. 이번 실행은 최대 " + gib(remain) + "의 데이터를 씁니다."
                : net.equals("NONE") ? "연결된 네트워크가 없습니다. 연결 후 다시 누르세요."
                : "Wi-Fi(종량제 아님)로 확인되지 않았습니다(" + net + "). 데이터 요금이 날 수 있습니다.";
        AlertDialog.Builder b = new AlertDialog.Builder(this)
            .setTitle("IPSW 전체를 끝까지 받습니다")
            .setMessage("대상: macOS Sequoia 15.6.1 (24G90) IPSW, 전체 16,814,137,790바이트(약 15.66 GiB)\n\n"
                + "지금까지 받은 양: " + gib(have) + " → 이번에 받을 양: 약 " + gib(remain) + "\n"
                + "네트워크 속도에 따라 수십 분 걸릴 수 있습니다. 받는 동안 화면이 꺼지지 않으니 앱을 앞에 두고 기다리세요(다른 앱으로 나가면 끊길 수 있습니다). 끊기면 자동으로 다시 연결하고, 안 되면 멈춘 곳에서 다시 이어받을 수 있습니다.\n\n"
                + "나가는 요청: Apple CDN(updates.cdn-apple.com, HTTPS·인증서 검증)에 Range 요청" + (have > 0 ? "(저장해 둔 ETag로 If-Range)" : "") + ". 이 폰의 IP 주소가 서버에 보입니다. ECID·서명 정보는 보내지 않습니다.\n\n"
                + "저장 위치: 앱 내부 저장소(files/vm). 사용 가능: " + (free < 0 ? "확인 못 함" : gib(free)) + "\n"
                + "네트워크: " + netText + "\n\n"
                + "전력·발열: 오래 화면을 켜 두므로 배터리가 줄고 폰이 뜨거워질 수 있습니다. 충전기를 연결하고, 많이 뜨거워지면 '다운로드 중단'을 누르세요.\n\n"
                + "무결성: 다 받은 뒤 SHA-256을 계산해 기대값(ipsw.me 한 곳에서만 확인한 값)과 같을 때만 최종 파일로 확정합니다. 다르면 파일이 손상된 것인지 기대값이 틀린 것인지 구분되지 않으며, 받은 파일은 남겨 둡니다.\n\n"
                + "서명(TSS)·복원·VM 실행은 하지 않습니다.")
            .setNegativeButton("취소", null)
            .setOnDismissListener(d -> dlDialogOpen = false);   // cancel, back, tap outside or start: the dialog is gone, the guard is released
        if (net.equals("NONE")) { b.show(); return; }
        b.setPositiveButton(wifi ? "받기 시작" : "그래도 받기", (d, w) -> runIpswDownload(exe, have, false));
        b.show();
    }

    private static String hms(double sec) {
        if (sec < 0 || Double.isNaN(sec) || Double.isInfinite(sec)) return "계산 중";
        long s = (long) sec;
        return s >= 3600 ? (s / 3600) + "시간 " + ((s % 3600) / 60) + "분" : s >= 60 ? (s / 60) + "분 " + (s % 60) + "초" : s + "초";
    }

    private void runIpswDownload(final File exe, final long have, final boolean verifyOnly) {
        if (running) return;
        running = true;
        setButtons(false);
        runOnUiThread(() -> {
            dlStopButton.setEnabled(true);
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);   // the screen stays on while the download runs
        });
        new Thread(() -> {
            final IpswDownload d = new IpswDownload(exe, ipswDir());
            ipswDl = d;
            final StringBuilder log = new StringBuilder();
            final String head = verifyOnly ? "이미 받은 최종 파일을 검증하는 중… (SHA-256 계산, 네트워크 없음)\n" : "IPSW 다운로드 중 (화면 켜짐 유지, 앱을 앞에 두세요)…\n";
            show(head);
            final long t0 = System.currentTimeMillis();
            final long[] last = {t0, -1};
            final double[] ema = {0};
            IpswDownload.Result r = d.run(0, new IpswDownload.Listener() {
                @Override public void onProgress(long done, long total) {
                    long now = System.currentTimeMillis();
                    if (last[1] >= 0 && now > last[0]) {
                        double inst = (done - last[1]) / ((now - last[0]) / 1000.0);
                        ema[0] = ema[0] == 0 ? inst : 0.8 * ema[0] + 0.2 * inst;
                    }
                    last[0] = now; last[1] = done;
                    String eta = ema[0] > 1 ? hms((total - done) / ema[0]) : "계산 중";
                    show(head + String.format(java.util.Locale.US, "받은 양: %.2f GiB / 전체 %.2f GiB (%.2f%%)\n속도: %.1f MB/s   남은 시간: %s\n경과: %s\n",
                            done / 1073741824.0, total / 1073741824.0, 100.0 * done / total, ema[0] / 1048576.0, eta, hms((now - t0) / 1000.0)) + log);
                }
                @Override public void onLine(String line) { if (!line.startsWith("dl: PROGRESS")) log.append("   ").append(line).append('\n'); }
            });
            String verdict;
            switch (r.code) {
                case 0: verdict = "완료: 전체를 받았고 SHA-256이 기대값과 일치해 최종 파일로 확정했습니다."; break;
                case 8: verdict = "사용자가 중단했습니다. 받은 부분은 일관된 상태로 남아 있습니다. 다시 누르면 이어받습니다."; break;
                case 5: verdict = "네트워크 문제 등으로 재연결 한도를 넘겨 멈췄습니다(또는 저장 오류). 받은 부분은 일관된 상태로 남아 있습니다. 다시 누르면 이어받습니다."; break;
                case 4: verdict = "서버 응답을 신뢰할 수 없어 멈췄습니다(범위·크기·ETag 불일치 또는 오류 코드). 받은 부분은 지우지 않았습니다. 아래 사유를 확인하세요."; break;
                case 3: verdict = "디스크의 부분 파일/메타데이터가 서로 맞지 않아 자동 이어받기를 하지 않았습니다. 아무것도 보내지 않았습니다. '받던 파일 삭제' 후 처음부터 받을 수 있습니다."; break;
                case 6: verdict = "전체를 받았지만 SHA-256이 기대값과 다릅니다. 최종 파일로 확정하지 않았고 받은 파일은 남겼습니다. 계산된 값과 기대값이 아래 로그에 있습니다."; break;
                case 2: verdict = "시작하지 않았습니다(공간 부족, 다른 다운로드 실행 중, 인자 오류 등). 아무것도 보내지 않았습니다."; break;
                default: verdict = "프로그램을 실행하지 못했거나 결과를 알 수 없습니다."; break;
            }
            show("IPSW 다운로드 결과: 코드 " + r.code + "\n" + verdict + "\n사유: " + r.text + "\n경과: " + hms((System.currentTimeMillis() - t0) / 1000.0) + "\n\n" + log);
            ipswDl = null;
            running = false;
            setButtons(true);
            runOnUiThread(() -> {
                dlStopButton.setEnabled(false);
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            });
        }).start();
    }

    private void showIpswStatus() {
        if (running) return;
        File exe = new File(getApplicationInfo().nativeLibraryDir, "libipswdl.so");
        if (!exe.exists()) { show("libipswdl.so가 이 APK에 없습니다.\n"); return; }
        new Thread(() -> {
            IpswDownload.Result r = new IpswDownload(exe, ipswDir()).status();
            long free;
            try { free = new StatFs(ipswDir().getAbsolutePath()).getAvailableBytes(); } catch (Throwable t) { free = -1; }
            show("IPSW 다운로드 상태\n" + r.detail + "\n(final=최종 파일 크기, part=받는 중인 파일, committed=확정 바이트, 없으면 -1, lock=held는 지금 받는 프로세스가 살아 있음, meta_age=메타데이터를 마지막으로 쓴 지 몇 초)\n"
                    + (r.detail.contains("lock=held") ? "지금 다른 곳에서 받는 중입니다. 'IPSW 다운로드'를 누르면 진행을 보여 줍니다(새로 시작하지 않습니다).\n" : "")
                    + "사용 가능한 공간: " + (free < 0 ? "확인 못 함" : gib(free)) + "\n");
        }).start();
    }

    private void confirmIpswDelete() {
        if (running || dlDialogOpen) return;
        File exe = new File(getApplicationInfo().nativeLibraryDir, "libipswdl.so");
        if (!exe.exists()) { show("libipswdl.so가 이 APK에 없습니다.\n"); return; }
        new AlertDialog.Builder(this)
            .setTitle("받던 파일을 삭제합니다")
            .setMessage("받던 파일(.part)과 이어받기 정보(.meta)를 지웁니다. 지운 뒤에는 처음부터 다시 받아야 합니다(지금까지 받은 양이 사라집니다). 이미 확정된 최종 IPSW 파일은 이 버튼으로 지워지지 않습니다.")
            .setPositiveButton("삭제", (d, w) -> new Thread(() -> {
                IpswDownload.Result r = new IpswDownload(exe, ipswDir()).deletePartial();
                show("받던 파일 삭제: 코드 " + r.code + "\n" + r.text + "\n");
            }).start())
            .setNegativeButton("취소", null)
            .show();
    }

    /** Runs libbmprobe.so: the approved BuildManifest-only partial read (see tests/bm_probe.c for the fixed limits). */
    private void runBuildManifestProbe() {
        if (running) return;
        running = true;
        setButtons(false);
        final String libDir = getApplicationInfo().nativeLibraryDir;
        new Thread(() -> {
            StringBuilder acc = new StringBuilder("BuildManifest 부분 조회 중… (최대 1분)\n");
            show(acc.toString());
            File exe = new File(libDir, "libbmprobe.so");
            if (!exe.exists()) acc.append("libbmprobe.so가 이 APK에 없습니다.\n");
            else acc.append(runTool(exe));
            show(acc.toString());
            running = false;
            setButtons(true);
        }).start();
    }

    private String runTool(File exe, String... args) {
        try {
            java.util.List<String> cmd = new java.util.ArrayList<>();
            cmd.add(exe.getAbsolutePath());
            for (String a : args) cmd.add(a);
            Process p = new ProcessBuilder(cmd).directory(getFilesDir()).redirectErrorStream(true).start();
            StringBuilder o = new StringBuilder();
            final boolean[] killed = {false};
            Thread dog = new Thread(() -> { // the read below blocks until the tool exits, so the time limit has to live elsewhere
                try { Thread.sleep(60000); killed[0] = true; p.destroyForcibly(); } catch (InterruptedException ignored) { }
            });
            dog.setDaemon(true);
            dog.start();
            try (java.io.BufferedReader br = new java.io.BufferedReader(new java.io.InputStreamReader(p.getInputStream()))) {
                String line;
                while ((line = br.readLine()) != null) o.append("   ").append(line).append('\n');
            }
            dog.interrupt();
            p.waitFor();
            if (killed[0]) o.append("   (60초 안에 끝나지 않아 종료)\n");
            return o.toString();
        } catch (Exception e) {
            return "   실행 오류: " + e + "\n";
        }
    }

    private void copyResult() {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        cm.setPrimaryClip(ClipData.newPlainText("henge-jit-probe", output.getText()));
        Toast.makeText(this, "복사했습니다", Toast.LENGTH_SHORT).show();
    }
}
