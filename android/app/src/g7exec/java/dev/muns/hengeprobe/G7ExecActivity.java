package dev.muns.hengeprobe;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * [G7-EXEC] Only in the g7exec build (never in the general APK). Shows what is there (files only: the binary is NOT executed to find out), and starts the G7 execution build
 * only through G7ExecLauncher, which refuses while the build's start switch is off (g7exec) and otherwise needs the person's consent (a checked box; nothing is typed) and the built-in IPSW SHA-256, which the runner verifies against the stored file.
 * No Intent extras are read. The ECID is never shown.
 */
public class G7ExecActivity extends Activity {
    private TextView status, log, summary;
    private CheckBox consent, isoConsent, netConsent;
    private android.widget.ProgressBar cloneBar;
    private Button start, cancel, refresh, share, copy, isolate, netStart, netStop, netShare, delSel, delAll, delCur, delClone, cloneBtn, cloneFromBtn, cbtn, cbStop, cbShare, cbLagfx, cbFrame, auxBtn, cbCopy, cbDump;
    private File pendingExport;   // the ZIP the user is about to save with the system file picker
    private static final int REQ_SAVE_ZIP = 77;
    private TextView cbMon; private ScrollView cbScroll;
    private TextView auxOut;
    // Restart after a guest SYSTEM_RESET (same clone, same options, never the E-mode NVRAM write again); only a confirmed clean stop, with a loop limit (G7RestartPolicy).
    private final G7RestartPolicy restartPolicy = new G7RestartPolicy();
    private File restartClone; private int restartUsb, restartSeconds = G7CloneBoot.RUN_SECONDS; private volatile boolean restartPending; private int restartGen;   // restartGen changes whenever a scheduled restart is cancelled or a run starts: an old callback then does nothing
    private ImageView cbImage; private TextView cbImageNote; private Object cbShownFrame;
    private static volatile boolean cloneCancel;   // static: the copy thread outlives the Activity; a recreated Activity's cancel button must still reach it
    private TextView storageInfo;
    private LinearLayout archiveBox;
    private final List<CheckBox> archiveBoxes = new ArrayList<>();
    private File privDir, nativeDirRef, extDirRef;
    /** The space a file really occupies (allocated blocks), not its 128 GiB logical size. */
    private static final G7Storage.Sizer SIZER = f -> { try { return android.system.Os.stat(f.getPath()).st_blocks * 512L; } catch (android.system.ErrnoException e) { throw new IOException(e.getMessage()); } };
    // No run state lives in this Activity: G7ExecLauncher.current() is the one source, so a recreated screen (rotation, back and forth) sees and can cancel the same process.

    @Override
    protected void onDestroy() { if (cloneDlg != null) { try { cloneDlg.dismiss(); } catch (RuntimeException ignored) { } cloneDlg = null; } super.onDestroy(); }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        if (b != null) {   // the system file picker can outlive this Activity (rotation, process death): the ZIP waiting to be saved is found again by its (validated) name
            String pz = b.getString("pendingExport");
            if (pz != null) pendingExport = G7ExecLauncher.logFile(getFilesDir(), pz);
        }
        if (android.os.Build.VERSION.SDK_INT >= 33 && checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) != android.content.pm.PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[]{android.Manifest.permission.POST_NOTIFICATIONS}, 7001);   // only so the foreground notice of a run is visible; the run works without it
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        final File ext = new File(getExternalFilesDir(null), "vm"), priv = new File(getFilesDir(), "vm");
        final File nativeDir = new File(getApplicationInfo().nativeLibraryDir);
        privDir = priv; nativeDirRef = nativeDir; extDirRef = ext;
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int p = (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 16, getResources().getDisplayMetrics());
        root.setPadding(p, p, p, p);
        add(root, "[G7-EXEC] G7 실제 복원 실행 (별도 빌드)", 18);
        add(root, "시작하면: 128 GiB 희소 설치 디스크와 AUX를 이 앱의 개인 저장소에 만들고, 쓰기 가능한 가상 머신을 띄워 Apple 서명 서버(gs.apple.com 등)에 요청하며 복원을 진행합니다. 수 시간이 걸릴 수 있습니다. "
                + "화면을 켠 채 이 앱을 앞에 두세요. 취소하거나 시스템이 앱을 끝내도 미완성 쌍은 in_progress로 남고, 자동으로 재개·삭제·덮어쓰지 않습니다.", 13);
        summary = new TextView(this);
        summary.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        summary.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        summary.setPadding(0, 0, 0, 24);
        root.addView(summary);
        status = new TextView(this);
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        status.setText(G7ExecLauncher.preflight(nativeDir, ext, priv).text);
        root.addView(status);
        cloneBar = new android.widget.ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal); cloneBar.setMax(100); cloneBar.setVisibility(View.GONE); root.addView(cloneBar);
        add(root, "IPSW: macOS 15.6.1 (24G90)\nSHA-256 무결성 검사: 자동. 시작할 때 앱이 파일의 SHA-256을 계산해 내장 참조값과 비교하고, 다르면 아무것도 만들지 않고 차단합니다(계산값과 참조값을 화면에 표시).", 13);
        consent = new CheckBox(this);
        consent.setText("설치 디스크·AUX 생성, 저장소 쓰기, Apple 서버 요청을 포함하는 실제 복원 실행에 동의합니다.");
        consent.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        root.addView(consent);
        start = new Button(this); start.setText("시작"); start.setEnabled(false); root.addView(start);
        consent.setOnCheckedChangeListener((v, on) -> { G7ExecLauncher.Run r = G7ExecLauncher.current(); start.setEnabled(on && (r == null || r.finished())); });
        cancel = new Button(this); cancel.setText("취소"); cancel.setEnabled(false); root.addView(cancel);
        refresh = new Button(this); refresh.setText("로그 새로고침"); root.addView(refresh);
        isoConsent = new CheckBox(this);
        isoConsent.setText("기존 설치 쌍(디스크·AUX·AUX 원본 복사·상태 파일)을 archives 폴더로 옮기는 데 동의합니다. 이름만 바꾸며 삭제·덮어쓰기·복사는 하지 않습니다.");
        isoConsent.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        root.addView(isoConsent);
        isolate = new Button(this); isolate.setText("기존 설치 쌍 격리 / 끝나지 않은 격리 복구"); isolate.setEnabled(false); root.addView(isolate);
        isoConsent.setOnCheckedChangeListener((v, on) -> isolate.setEnabled(on));
        isolate.setOnClickListener(v -> {
            final java.util.function.IntSupplier st = () -> G7ExecLauncher.runnerStatus(android.os.Process.myPid(), getFilesDir());   // evaluated inside the start lock
            G7Isolation.Result r = G7Isolation.pending(priv) ? G7Isolation.recover(priv, st) : G7Isolation.isolate(priv, st);
            status.setText(r.message + "\n\n" + G7ExecLauncher.preflight(nativeDir, ext, priv).text);
            isoConsent.setChecked(false);   // every isolation needs a fresh consent
            showSummary();
            refreshStorage();
        });
        add(root, "[저장공간 관리] 격리된 설치 쌍과 지금 남아 있는 실패한 설치 쌍을 삭제해 공간을 확보합니다. 삭제는 되돌릴 수 없고, 확인 창에서 '삭제'를 눌러야만 실행됩니다(자동 삭제 없음). 지우는 것은 디스크·AUX·AUX 원본 복사·상태 파일·격리 메모뿐이며 ROM·IPSW·ECID·로그는 지우지 않습니다. 복원이 실행 중이면 삭제할 수 없고, 정상 설치가 끝난 디스크(state=done)는 지우지 않습니다.", 13);
        storageInfo = new TextView(this);
        storageInfo.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        root.addView(storageInfo);
        archiveBox = new LinearLayout(this);
        archiveBox.setOrientation(LinearLayout.VERTICAL);
        root.addView(archiveBox);
        delSel = new Button(this); delSel.setText("선택 삭제"); root.addView(delSel);
        delAll = new Button(this); delAll.setText("격리 파일 전체 삭제"); root.addView(delAll);
        delCur = new Button(this); delCur.setText("현재 실패한 설치 쌍 바로 삭제(격리하지 않음)"); root.addView(delCur);
        delSel.setOnClickListener(v -> {
            List<String> names = new ArrayList<>();
            for (CheckBox c : archiveBoxes) if (c.isChecked() && c.isEnabled()) names.add((String) c.getTag());
            if (names.isEmpty()) { status.setText("삭제할 보관 폴더를 선택하세요."); return; }
            confirmDeleteArchives(names);
        });
        delAll.setOnClickListener(v -> {
            List<String> names = new ArrayList<>();
            for (G7Storage.Item it : G7Storage.list(privDir, SIZER)) if (it.deletable) names.add(it.name);
            if (names.isEmpty()) { status.setText("삭제할 수 있는 격리 폴더가 없습니다."); return; }
            confirmDeleteArchives(names);
        });
        delCur.setOnClickListener(v -> confirmDeleteCurrent());
        delClone = new Button(this); delClone.setText("복제본 하나 삭제(vm/clones/, 설치 쌍은 그대로)"); root.addView(delClone);
        delClone.setOnClickListener(v -> confirmDeleteClone());
        cloneBtn = new Button(this); cloneBtn.setText("설치 쌍 희소 복제본 만들기(원본은 읽기만)"); root.addView(cloneBtn);
        cloneBtn.setOnClickListener(v -> confirmClone());
        cloneFromBtn = new Button(this); cloneFromBtn.setText("기존 복제본에서 복제본 만들기(그 복제본은 읽기만)"); root.addView(cloneFromBtn);
        cloneFromBtn.setOnClickListener(v -> confirmCloneFrom());
        add(root, "[복제본 부팅] 검증된 복제본(vm/clones/)을 설치 단계가 아닌 일반 부팅으로 시험합니다. 이 복제본의 디스크·AUX만 쓰며 원본 설치 쌍은 사용하지 않습니다(최대 15분, 취소 가능). 화면이 나왔는지는 이 앱에서 '화면 프레임 수'와 부팅 단계 추정으로만 보입니다(실측 아님).", 13);
        cbtn = new Button(this); cbtn.setText("복제본 부팅 시작"); root.addView(cbtn);
        cbStop = new Button(this); cbStop.setText("복제본 부팅 중지"); cbStop.setEnabled(false); root.addView(cbStop);
        cbShare = new Button(this); cbShare.setText("복제본 부팅 로그 공유(전체 파일)"); root.addView(cbShare);
        cbLagfx = new Button(this); cbLagfx.setText("lagfx(GPU) 로그 공유(전체 파일)"); root.addView(cbLagfx);
        cbLagfx.setOnClickListener(v -> {   // g7_lagfx_*.txt exists only after a run of the lagfx build of the runner
            File f = G7CloneBoot.newestLagfxLog(getFilesDir());
            if (f == null) { status.setText("공유할 lagfx 로그 파일이 없습니다(g7_lagfx_*.txt 없음: lagfx 빌드로 복제본 부팅을 한 번 실행해야 생깁니다)."); return; }
            try { shareFile(f, "lagfx 로그 공유"); } catch (RuntimeException e) { status.setText("lagfx 로그 공유 실패: " + e.getClass().getSimpleName() + " " + e.getMessage()); }
        });
        cbDump = new Button(this); cbDump.setText("GPU 덤프 + 로그 ZIP 내보내기(최신 실행 기본, 이전 실행 선택 가능)"); root.addView(cbDump);
        cbDump.setOnClickListener(v -> chooseDumpExport());
        Button pipeRepro = new Button(this); pipeRepro.setText("GPU 파이프라인 재현 시험(VM 부팅 없음)"); root.addView(pipeRepro);
        pipeRepro.setOnClickListener(v -> choosePipeRepro());
        cbCopy = new Button(this); cbCopy.setText("복제본 부팅 로그 끝부분 복사"); root.addView(cbCopy);
        add(root, "아래 상자는 '복제본 부팅'의 상태·로그입니다(복원 로그와 다름). 반복되는 USB 진단 줄은 화면에서만 묶어 보이며 로그 파일에는 모두 남습니다.", 12);
        cbMon = new TextView(this); cbMon.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11); cbMon.setTypeface(android.graphics.Typeface.MONOSPACE); cbMon.setTextIsSelectable(true);
        cbScroll = new ScrollView(this); cbScroll.addView(cbMon);
        cbScroll.setOnTouchListener((v, e) -> { v.getParent().requestDisallowInterceptTouchEvent(true); return false; });
        root.addView(cbScroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, (int) (getResources().getDisplayMetrics().density * 360)));
        cbFrame = new Button(this); cbFrame.setText("지금 화면 확인(러너에 요청)"); root.addView(cbFrame);
        cbFrame.setOnClickListener(v -> { G7CloneBoot.Run r = G7CloneBoot.current(); if (r == null || r.finished()) status.setText("실행 중인 복제본 부팅이 없습니다."); else { r.requestFrame(); status.setText("화면을 요청했습니다. 러너 응답이 아래(이미지 자리)에 표시됩니다. 화면이 아직 없으면 그 이유가 나옵니다."); } });
        cbImage = new ImageView(this); cbImage.setAdjustViewBounds(true); cbImage.setMinimumHeight(1); root.addView(cbImage);
        cbImageNote = new TextView(this); cbImageNote.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12); root.addView(cbImageNote);
        add(root, "[AUX 비교] 복원 전(macos_aux.initial)과 복원 후(macos_aux.img)의 AUX를 읽기 전용으로 비교하고, 복제본들의 AUX가 원본과 같은지도 봅니다. 아무것도 쓰거나 바꾸지 않습니다.", 13);
        auxBtn = new Button(this); auxBtn.setText("AUX 비교 실행(읽기 전용)"); root.addView(auxBtn);
        auxOut = new TextView(this); auxOut.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12); auxOut.setTextIsSelectable(true); root.addView(auxOut);
        add(root, "[Vulkan 장치 탐지] 이 폰의 Vulkan 장치와 기능을 읽기만 합니다(VkInstance 하나를 만들고 묻고 닫음). VM과 무관하며 아무것도 그리거나 쓰지 않습니다. GPU 이식(docs/gpu-research S5)의 근거 수집용입니다.", 13);
        final Button vkBtn = new Button(this); vkBtn.setText("Vulkan 장치 탐지(읽기 전용)"); root.addView(vkBtn);
        final TextView vkOut = new TextView(this); vkOut.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12); vkOut.setTextIsSelectable(true); root.addView(vkOut);
        vkBtn.setOnClickListener(v -> {
            vkBtn.setEnabled(false); vkOut.setText("탐지하는 중…");
            new Thread(() -> {
                final String text = "SDK " + android.os.Build.VERSION.SDK_INT + ", " + android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL + "\n" + VulkanProbe.report();
                runOnUiThread(() -> { vkOut.setText(text); vkBtn.setEnabled(true); });
            }, "g7-vulkan-probe").start();
        });
        auxBtn.setOnClickListener(v -> {
            auxBtn.setEnabled(false); auxOut.setText("비교하는 중…");
            new Thread(() -> {
                StringBuilder o = new StringBuilder(G7AuxCompare.compareInitialToImage(privDir));
                o.append("\n[복제본 AUX ↔ 원본 복원 후 AUX]\n");
                for (File c : G7CloneBoot.listValid(privDir)) o.append(G7AuxCompare.compareCloneToOrigin(privDir, c)).append('\n');
                final String text = o.toString();
                runOnUiThread(() -> { auxOut.setText(text); auxBtn.setEnabled(true); });
            }, "g7-aux-compare").start();
        });
        cbtn.setOnClickListener(v -> confirmCloneBoot());
        cbStop.setOnClickListener(v -> {
            boolean wasPending = restartPending;
            restartPending = false; restartClone = null; restartGen++;
            G7CloneBoot.Run r = G7CloneBoot.current();
            if (wasPending) { cbtn.setEnabled(true); cbStop.setEnabled(false); G7RunService.stop(this); status.setText("예약된 자동 재시작을 취소했습니다."); }
            else if (r != null) { r.cancel(); status.setText("복제본 부팅 중지를 요청했습니다."); }
        });
        cbShare.setOnClickListener(v -> {   // the file on disk, not the Run object: still works after the app was restarted
            G7CloneBoot.Run r = G7CloneBoot.current();
            File f = r != null ? G7ExecLauncher.logFile(getFilesDir(), r.logName()) : null;
            if (f == null) f = G7CloneBoot.newestCloneLog(getFilesDir());
            if (f == null) { status.setText("공유할 복제본 부팅 로그 파일이 없습니다(g7_cboot_*.txt 없음)."); return; }
            try { shareFile(f, "복제본 부팅 로그 공유"); } catch (RuntimeException e) { status.setText("복제본 로그 공유 실패: " + e.getClass().getSimpleName() + " " + e.getMessage()); }
        });
        cbCopy.setOnClickListener(v -> {
            String t = G7CloneBoot.readSavedCloneLog(getFilesDir());
            if (t.isEmpty()) { status.setText("복사할 복제본 부팅 로그가 없습니다(g7_cboot_*.txt 없음 또는 비어 있음)."); return; }
            if (t.length() > 100000) t = t.substring(t.length() - 100000);
            try { ((ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("g7 clone boot log", t)); status.setText("복제본 부팅 로그 끝부분 " + t.length() + "자를 복사했습니다(복원 로그 아님)."); }
            catch (RuntimeException e) { status.setText("복사 실패: " + e.getClass().getSimpleName() + " " + e.getMessage() + " (공유 버튼으로 파일을 보내세요)"); }
        });
        share = new Button(this); share.setText("복원 로그 파일 공유(전체)"); root.addView(share);
        copy = new Button(this); copy.setText("복원 로그 끝부분 복사"); root.addView(copy);
        add(root, "[네트워크 시험] Apple 서명 서버 주소 3개에 이름 조회·TCP 연결·TLS 핸드셰이크만 시험합니다(요청 본문 전송 없음, 최대 약 5분, 복원 없음). 복원이 실행 중일 때도 시작할 수 있어 비교에 쓸 수 있습니다.", 13);
        netConsent = new CheckBox(this);
        netConsent.setText("Apple 서버 3개 주소에 이름 조회·연결·TLS 핸드셰이크만 하는 네트워크 시험에 동의합니다(요청 본문 전송 없음).");
        netConsent.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        root.addView(netConsent);
        netStart = new Button(this); netStart.setText("네트워크 시험 시작"); netStart.setEnabled(false); root.addView(netStart);
        netStop = new Button(this); netStop.setText("네트워크 시험 중지"); netStop.setEnabled(false); root.addView(netStop);
        netShare = new Button(this); netShare.setText("네트워크 시험 로그 공유"); root.addView(netShare);
        netConsent.setOnCheckedChangeListener((v, on) -> { G7NetProbe.Run r = G7NetProbe.current(); netStart.setEnabled(on && (r == null || r.finished())); });
        netStart.setOnClickListener(v -> {
            G7NetProbe.Start ns = G7NetProbe.start(nativeDir, getFilesDir(), new G7NetState(this), netConsent.isChecked());
            status.setText(ns.message);
            netConsent.setChecked(false);   // every probe needs a fresh consent
            if (ns.started) { netStart.setEnabled(false); netStop.setEnabled(true); netPoll(); }
        });
        netStop.setOnClickListener(v -> { G7NetProbe.Run r = G7NetProbe.current(); if (r != null) { r.cancel(); status.setText("네트워크 시험 중지를 요청했습니다."); } });
        netShare.setOnClickListener(v -> shareFile(G7NetProbe.newestLog(getFilesDir()), "네트워크 시험 로그 공유"));
        log = new TextView(this);
        log.setTextIsSelectable(true);
        log.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        log.setTypeface(android.graphics.Typeface.MONOSPACE);
        log.setText(G7ExecLauncher.readLogTail(getFilesDir(), 6000));
        root.addView(log);
        showSummary();
        showCloneBox(G7CloneBoot.current());
        refreshStorage();
        String note = G7ExecLauncher.lastRunNote(getFilesDir());
        if (!note.isEmpty()) status.setText(status.getText() + "\n\n" + note);
        G7ExecLauncher.Run live = G7ExecLauncher.current();
        if (live != null && !live.finished()) { start.setEnabled(false); cancel.setEnabled(true); poll(); }
        if (G7CloneProgress.current().running) {   // a clone is running (Activity recreated): same popup, same locks against a second clone
            setDeleteButtons(false); cloneBtn.setEnabled(false); cloneFromBtn.setEnabled(false); cancel.setEnabled(true); showCloneProgress();
        }
        G7CloneBoot.Run cLive = G7CloneBoot.current();
        if (cLive != null && !cLive.finished()) { cbtn.setEnabled(false); cbStop.setEnabled(true); cbootPoll(); }
        G7NetProbe.Run netLive = G7NetProbe.current();
        if (netLive != null && !netLive.finished()) { netStop.setEnabled(true); netPoll(); }
        start.setOnClickListener(v -> {
            G7ExecLauncher.Start s = G7ExecLauncher.start(nativeDir, ext, priv, getFilesDir(), consent.isChecked());
            status.setText(s.message);
            consent.setChecked(false);   // every start needs a fresh consent
            if (s.started) { start.setEnabled(false); cancel.setEnabled(true); poll(); }
        });
        cancel.setOnClickListener(v -> { cloneCancel = true; G7ExecLauncher.Run r = G7ExecLauncher.current(); if (r != null) { r.cancel(); status.setText("취소를 요청했습니다(복원을 멈추고 쌍은 in_progress로 보존)."); } });
        refresh.setOnClickListener(v -> { G7ExecLauncher.Run r = G7ExecLauncher.current(); log.setText(r != null ? r.tail() : G7ExecLauncher.readLogTail(getFilesDir(), 6000)); showSummary(); });
        share.setOnClickListener(v -> shareLog());
        copy.setOnClickListener(v -> {
            String t = G7ExecLauncher.readLogTail(getFilesDir(), 100000);
            if (t.isEmpty()) { status.setText("복사할 로그가 없습니다."); return; }
            ((ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("g7 log", t));
            status.setText("로그 끝부분 " + t.length() + "자를 복사했습니다.");
        });
        ScrollView sv = new ScrollView(this);
        sv.addView(root);
        setContentView(sv);
    }


    private java.util.function.IntSupplier runnerSt() { return () -> G7ExecLauncher.runnerStatus(android.os.Process.myPid(), getFilesDir()); }   // evaluated inside the start lock

    private static String when(long ms) { return ms <= 0 ? "날짜 미상" : new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm", java.util.Locale.US).format(new java.util.Date(ms)); }

    /** Free space and the list of isolated pairs with the space they really occupy; read again after every deletion. */
    private void refreshStorage() {
        long free = privDir.getUsableSpace();
        StringBuilder t = new StringBuilder("사용 가능한 저장공간: ").append(G7Storage.fmt(free)).append("  (새 복원 시작에 최소 ").append(G7Storage.fmt(G7Storage.MIN_FREE_BYTES)).append(" 필요, 추정값)");
        long cur = 0; boolean have = false;
        for (String n : G7Isolation.NAMES) { File f = new File(privDir, n); if (f.isFile()) { have = true; try { cur += SIZER.allocated(f); } catch (IOException e) { /* shown as less */ } } }
        t.append("\n현재 설치 쌍(vm/): ").append(have ? "실제 " + G7Storage.fmt(cur) + " 사용, 상태 " + G7Storage.stateOf(new File(privDir, "g7_state.txt")) : "없음");
        archiveBox.removeAllViews(); archiveBoxes.clear();
        List<G7Storage.Item> items = G7Storage.list(privDir, SIZER);
        t.append("\n격리된 설치 파일: ").append(items.isEmpty() ? "없음" : items.size() + "개");
        storageInfo.setText(t);
        for (G7Storage.Item it : items) {
            CheckBox c = new CheckBox(this);
            c.setText(it.name + "\n" + when(it.createdMs) + " · 실제 " + G7Storage.fmt(it.allocated) + " · " + it.state + (it.deletable ? "" : "\n(" + it.note + ")"));
            c.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
            c.setTag(it.name); c.setEnabled(it.deletable);
            archiveBox.addView(c); archiveBoxes.add(c);
        }
    }

    private void setDeleteButtons(boolean on) { delSel.setEnabled(on); delAll.setEnabled(on); delCur.setEnabled(on); delClone.setEnabled(on); }

    private void confirmDeleteArchives(final List<String> names) {
        long total = 0;
        for (G7Storage.Item it : G7Storage.list(privDir, SIZER)) if (names.contains(it.name)) total += it.allocated;
        final long shown = total;
        new AlertDialog.Builder(this).setTitle("영구 삭제 확인")
                .setMessage(names.size() + "개 보관 폴더(" + String.join(", ", names) + ")의 디스크·AUX·상태 파일을 영구 삭제합니다. 약 " + G7Storage.fmt(shown) + "을 되찾습니다. 되돌릴 수 없습니다.")
                .setNegativeButton("취소", null)
                .setPositiveButton("삭제", (d, w) -> runDelete(() -> G7Storage.deleteArchives(privDir, names, runnerSt(), SIZER).message)).show();
    }

    /** The data extents of a file by lseek(SEEK_DATA = 3, SEEK_HOLE = 4). Anything unexpected is "sparse-unsupported": the clone then copies nothing. */
    private static final G7Clone.Extents EXTENTS = (f, size) -> {
        List<long[]> out = new ArrayList<>();
        java.io.FileDescriptor fd = null;
        try {
            fd = android.system.Os.open(f.getPath(), android.system.OsConstants.O_RDONLY, 0);
            long pos = 0;
            while (pos < size) {
                long ds;
                try { ds = android.system.Os.lseek(fd, pos, 3); }
                catch (android.system.ErrnoException e) { if (e.errno == android.system.OsConstants.ENXIO) break; throw new IOException("sparse-unsupported"); }
                long he = android.system.Os.lseek(fd, ds, 4);
                if (he > size) he = size;
                if (ds < pos || he <= ds) throw new IOException("sparse-unsupported");
                out.add(new long[]{ds, he}); pos = he;
            }
        } catch (android.system.ErrnoException e) { throw new IOException("sparse-unsupported");
        } finally { if (fd != null) { try { android.system.Os.close(fd); } catch (android.system.ErrnoException ignored) { } } }
        return out;
    };

    private void confirmClone() {
        new AlertDialog.Builder(this).setTitle("희소 복제본 만들기")
                .setMessage("지금 vm/의 설치 쌍(디스크·AUX)을 vm/clones/clone-NNN/ 로 복제합니다. 원본은 읽기만 하고 바꾸지 않으며 상태(in_progress 또는 done)도 그대로입니다. 디스크의 구멍은 그대로 두고 실제 데이터만 복사하고(구멍 정보가 없는 파일시스템이면 복사하지 않습니다), 끝나면 데이터를 다시 읽어 검증합니다. 여유 공간이 복제본 크기 + 8GiB 이상 필요하고 몇 분 걸릴 수 있습니다. 취소 버튼으로 중단할 수 있습니다. 복제본 부팅은 아래 '복제본 부팅' 항목에서 따로 시작합니다.")
                .setNegativeButton("취소", null)
                .setPositiveButton("복제", (d, w) -> { cloneCancel = false; runClone(); }).show();
    }

    private AlertDialog cloneDlg;
    private android.widget.TextView cloneDlgText;
    private android.widget.ProgressBar cloneDlgBar;

    /** Shows the running clone in a popup (also after the Activity was recreated: the state is in G7CloneProgress); polls every 500 ms until it ends, then shows the result with a close button. Used by both clone paths. */
    private void showCloneProgress() {
        final G7CloneProgress pg = G7CloneProgress.current();
        cloneBar.setVisibility(View.VISIBLE);
        cloneBar.setProgress(pg.percent());
        final String text = pg.render(System.currentTimeMillis());
        status.setText(text);
        if (cloneDlg == null || !cloneDlg.isShowing()) {
            if (!pg.running && pg.ok != null && cloneDlg == null) return;   // finished earlier and already acknowledged: no popup
            cloneDlgText = new android.widget.TextView(this); cloneDlgText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14); cloneDlgText.setPadding(0, 0, 0, 16);
            cloneDlgBar = new android.widget.ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal); cloneDlgBar.setMax(100);
            LinearLayout box = new LinearLayout(this); box.setOrientation(LinearLayout.VERTICAL); box.setPadding(48, 24, 48, 0);
            box.addView(cloneDlgText); box.addView(cloneDlgBar);
            cloneDlg = new AlertDialog.Builder(this).setTitle("복제 진행").setView(box).setCancelable(false)
                    .setNegativeButton("취소", null).create();
            cloneDlg.show();
            // the cancel button must not dismiss the popup: the copy stops by itself and the result is shown here
            cloneDlg.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener(v -> {
                if (G7CloneProgress.current().running) { cloneCancel = true; ((Button) v).setEnabled(false); cloneDlgText.append("\n취소를 요청했습니다. 진행 중인 복사를 안전하게 멈추는 중입니다."); }
            });
        }
        cloneDlgBar.setProgress(pg.percent());
        if (pg.running) {
            cloneDlgText.setText(text);
            log.postDelayed(this::showCloneProgress, 500);
        } else {
            cloneDlgText.setText((pg.ok != null && pg.ok ? "복제가 끝났습니다.\n" : "복제가 끝나지 않았습니다.\n") + pg.message + "\n\n" + text);
            Button b = cloneDlg.getButton(AlertDialog.BUTTON_NEGATIVE);
            b.setText("닫기"); b.setEnabled(true);
            b.setOnClickListener(v -> { cloneDlg.dismiss(); cloneDlg = null; });
        }
    }

    private void confirmCloneFrom() {
        final List<File> clones = G7CloneBoot.listValid(privDir);
        if (clones.isEmpty()) { status.setText("복제 원본으로 쓸 검증된 복제본이 없습니다."); return; }
        final String[] names = new String[clones.size()];
        for (int i = 0; i < names.length; i++) names[i] = G7CloneHistory.label(getFilesDir(), privDir, clones.get(i));
        new AlertDialog.Builder(this).setTitle("복제할 기존 복제본 고르기")
                .setItems(names, (d, which) -> {
                    final File from = clones.get(which);
                    new AlertDialog.Builder(this).setTitle(from.getName() + " 을(를) 복제")
                            .setMessage(from.getName() + "의 지금 디스크·AUX 상태를 vm/clones/clone-NNN/ 로 복제합니다. " + from.getName() + "와 설치 원본은 읽기만 하고 바꾸지 않습니다. 복제 도중 그 복제본이 바뀌면 복제하지 않습니다. 그 복제본의 지금 디스크 상태 그대로 복사됩니다(게스트 macOS를 정상 종료하지 않고 VM만 멈췄다면 게스트 파일시스템이 정상 종료 상태가 아닐 수 있습니다). 정상 부팅이 확인된 복제본이라는 뜻은 아닙니다(구조·ECID 검증만 통과한 것). 여유 공간이 복제본 크기 + 8GiB 이상 필요하고 몇 분 걸릴 수 있습니다.")
                            .setNegativeButton("취소", null)
                            .setPositiveButton("복제", (dd, ww) -> { cloneCancel = false; runCloneFrom(from); }).show();
                })
                .setNegativeButton("취소", null).show();
    }

    private void runCloneFrom(final File from) {
        setDeleteButtons(false); cloneBtn.setEnabled(false); cloneFromBtn.setEnabled(false); cancel.setEnabled(true);
        final G7CloneProgress pg = G7CloneProgress.begin(from.getName(), System.currentTimeMillis()); showCloneProgress();
        new Thread(() -> {
            String m; boolean good = false;
            try {
                java.io.File priv = privDir.getCanonicalFile();
                G7Clone.Result r = G7Clone.cloneFromClone(from, G7ProbeLauncher.readEcid(priv), priv, runnerSt(), EXTENTS, SIZER, privDir::getUsableSpace, () -> cloneCancel, pg); m = r.message; good = r.ok;
            } catch (java.io.IOException | RuntimeException e) { m = "복제 중 예기치 못한 오류가 났습니다. 원본은 바뀌지 않았습니다."; }
            pg.finish(good, m);
            final String msg = m;
            runOnUiThread(() -> { refreshStorage(); showSummary(); setDeleteButtons(true); cloneBtn.setEnabled(true); cloneFromBtn.setEnabled(true); cancel.setEnabled(false); status.setText(msg); });
        }, "g7-clone-from").start();
    }

    private void confirmCloneBoot() {
        final List<File> clones = G7CloneBoot.listValid(privDir);
        if (clones.isEmpty()) {
            List<String> all = G7CloneBoot.describeAll(privDir);
            status.setText("부팅할 수 있는 검증된 복제본이 없습니다." + (all.isEmpty() ? "(복제본을 먼저 만드세요)" : "\n" + android.text.TextUtils.join("\n", all)));
            return;
        }
        final String[] names = new String[clones.size()];
        for (int i = 0; i < names.length; i++) names[i] = G7CloneHistory.label(getFilesDir(), privDir, clones.get(i));
        final int[] pick = {0};
        final int pad = (int) (8 * getResources().getDisplayMetrics().density);
        final String[] um = {"A. USB 없음", "B. USB 포트만", "C. 포트 + 초기 열거", "D. C + getenv 조회", "E. D + auto-boot 저장"};
        final String[] lm = {"3분", "15분", "30분", "1시간", "제한 없음"};
        final android.widget.Spinner cloneSp = new android.widget.Spinner(this); cloneSp.setAdapter(new android.widget.ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, names));
        cloneSp.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> p, View v, int i, long id) { pick[0] = i; }
            @Override public void onNothingSelected(android.widget.AdapterView<?> p) { }
        });
        final android.widget.Spinner usbSp = new android.widget.Spinner(this); usbSp.setAdapter(new android.widget.ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, um));
        final android.widget.Spinner limSp = new android.widget.Spinner(this); limSp.setAdapter(new android.widget.ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, lm));
        limSp.setSelection(0);   // 3 minutes: the length the GPU tests use (index into G7CloneBoot.RUN_SECONDS_CHOICES)
        final android.widget.RadioGroup gw = new android.widget.RadioGroup(this); gw.setOrientation(android.widget.RadioGroup.HORIZONTAL);
        final String[] gwl = {"끔", "즉시", "지연"};
        for (int i = 0; i < gwl.length; i++) { android.widget.RadioButton rb = new android.widget.RadioButton(this); rb.setText(gwl[i]); rb.setId(3000 + i); rb.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14); gw.addView(rb); }
        gw.check(3001);   // the default the earlier checkbox had: immediate
        final android.content.SharedPreferences opts = getSharedPreferences(OPTS_PREFS, MODE_PRIVATE);
        final boolean gxWas = opts.getBoolean(KEY_GPU_EXEC, false);   // the last value used for a start; the default (nothing saved yet) is OFF
        final CheckBox gx = shortBox("GPU 실제 실행"); gx.setChecked(gxWas);
        final TextView gxState = new TextView(this); gxState.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        final Runnable gxShow = () -> { boolean on = gx.isChecked(); gxState.setText(on ? "현재: 켜짐 (Render/Blit를 Vulkan으로 실제 실행)" + (gxWas ? " - 지난 실행에서 켠 값을 불러옴" : "") : "현재: 꺼짐 (해석만 함, 기본값)"); gxState.setTextColor(on ? 0xFFB00020 : 0xFF555555); };
        gx.setOnCheckedChangeListener((b, c) -> gxShow.run()); gxShow.run();
        final CheckBox pat = shortBox("화면 시험 패턴");
        final CheckBox de = shortBox("디스플레이 이벤트(VBL)");
        final CheckBox gd = shortBox("셰이더·파이프라인 저장");
        final boolean cdcWas = opts.getBoolean(KEY_REPLAY, false);   // default OFF; the last value used for a start
        final CheckBox cdc = shortBox("Render Replay 캡처 (LAGFX_ARM64_DUMP_DATA=1)"); cdc.setChecked(cdcWas);
        final TextView cdcState = new TextView(this); cdcState.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        final CheckBox dd = shortBox("그리기별 진단 (G7_DRAW_DIAG)"); dd.setChecked(opts.getBoolean(KEY_DRAW_DIAG, true));   // default ON, independent of Render Replay
        final TextView ddState = new TextView(this); ddState.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        final Runnable ddShow = () -> ddState.setText(dd.isChecked() ? "현재: 켜짐 (draw별 요약 로그, 용량 제한, 기본값)" : "현재: 꺼짐 (G7_DRAW_DIAG=0)");
        dd.setOnCheckedChangeListener((b, c) -> ddShow.run()); ddShow.run();
        final CheckBox ok = shortBox("복제본 디스크·AUX 쓰기 동의(원본은 쓰지 않음)");
        final LinearLayout box = new LinearLayout(this); box.setOrientation(LinearLayout.VERTICAL); box.setPadding(pad * 2, pad, pad * 2, pad);
        box.addView(labelRow("복제본", cloneSp)); box.addView(labelRow("USB 모드", usbSp)); box.addView(labelRow("실행 시간", limSp));
        box.addView(sectionLabel("GPU")); box.addView(gx); box.addView(gxState);
        box.addView(labelRow("화면 되쓰기", gw));
        box.addView(sectionLabel("실험 및 진단")); box.addView(pat); box.addView(de); box.addView(gd); box.addView(dd); box.addView(ddState); box.addView(cdc); box.addView(cdcState);
        final Runnable cdcShow = () -> {
            boolean want = cdc.isChecked(), active = want && gx.isChecked() && gd.isChecked();
            cdcState.setText(!want ? "현재: 꺼짐 (기본값)" : active ? "현재: 켜짐 - draw별 버퍼·텍스처·어태치먼트 저장(최대 256 draw·128 MiB)" + (cdcWas ? " - 지난 실행에서 켠 값을 불러옴" : "")
                    : "현재: 켜짐으로 선택했지만 적용되지 않음 - GPU 실제 실행과 셰이더·파이프라인 저장도 켜야 합니다");
            cdcState.setTextColor(active ? 0xFFB00020 : want ? 0xFF9A6700 : 0xFF555555);
        };
        cdc.setOnCheckedChangeListener((b, c) -> cdcShow.run()); gx.setOnCheckedChangeListener((b, c) -> { gxShow.run(); cdcShow.run(); }); gd.setOnCheckedChangeListener((b, c) -> cdcShow.run()); cdcShow.run();
        box.addView(sectionLabel("동의")); box.addView(ok);
        final android.widget.ScrollView sv = new android.widget.ScrollView(this); sv.addView(box);   // the dialog's own buttons stay at the bottom; only the options scroll
        final AlertDialog dlg = new AlertDialog.Builder(this).setTitle("복제본 부팅").setView(sv)
                .setNegativeButton("취소", null)
                .setNeutralButton("도움말", null)
                .setPositiveButton("부팅 시작", (d, w) -> {
                    int mode = Math.max(0, Math.min(4, usbSp.getSelectedItemPosition()));
                    int limIdx = Math.max(0, Math.min(G7CloneBoot.RUN_SECONDS_CHOICES.length - 1, limSp.getSelectedItemPosition()));
                    final int seconds = G7CloneBoot.RUN_SECONDS_CHOICES[limIdx];
                    G7Storage.setPatternMarker(privDir, pat.isChecked());
                    G7CloneBoot.gpuExec = gx.isChecked();
                    opts.edit().putBoolean(KEY_GPU_EXEC, gx.isChecked()).apply();   // remembered for the next dialog (a cancelled dialog saves nothing)
                    G7CloneBoot.displayEvents = de.isChecked();
                    G7CloneBoot.captureData = cdc.isChecked();
                    G7CloneBoot.drawDiag = dd.isChecked();
                    opts.edit().putBoolean(KEY_DRAW_DIAG, dd.isChecked()).apply();
                    opts.edit().putBoolean(KEY_REPLAY, cdc.isChecked()).apply();
                    G7CloneBoot.gpuWritebackMode = Math.max(0, Math.min(2, gw.getCheckedRadioButtonId() - 3000));
                    File xd = getExternalFilesDir(null);
                    G7CloneBoot.gpuDumpDir = gd.isChecked() ? new File(xd != null ? xd : getFilesDir(), "g7_gpudump") : null;
                    G7CloneBoot.Start st = G7CloneBoot.start(nativeDirRef, extDirRef, privDir, getFilesDir(), clones.get(pick[0]), ok.isChecked(), runnerSt(), mode, seconds);
                    status.setText(st.message);
                    if (st.started) {
                        restartPolicy.reset(); restartGen++; restartClone = clones.get(pick[0]); restartUsb = mode == G7CloneBoot.USB_AUTOBOOT_FIX ? G7CloneBoot.USB_PORT_GETENV : mode; restartSeconds = seconds; restartPending = false;
                        cbtn.setEnabled(false); cbStop.setEnabled(true); startRunService(); cbootPoll();
                    }
                }).create();
        wireHelp(dlg);
        dlg.show();
    }

    /** Boot-dialog options that are remembered between starts (SharedPreferences, private to the app). Every default is OFF. */
    static final String OPTS_PREFS = "g7exec_boot_options", KEY_GPU_EXEC = "gpu_exec", KEY_REPLAY = "render_replay", KEY_DRAW_DIAG = "draw_diag";   // the write consent is NOT here: it is ticked by hand for every run

    private CheckBox shortBox(String text) { CheckBox c = new CheckBox(this); c.setText(text); c.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14); return c; }
    private TextView sectionLabel(String text) { TextView t = new TextView(this); t.setText(text); t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12); t.setTypeface(null, android.graphics.Typeface.BOLD); t.setPadding(0, (int) (10 * getResources().getDisplayMetrics().density), 0, 0); return t; }
    private LinearLayout labelRow(String label, View field) {
        LinearLayout r = new LinearLayout(this); r.setOrientation(LinearLayout.HORIZONTAL); r.setGravity(android.view.Gravity.CENTER_VERTICAL);
        TextView t = new TextView(this); t.setText(label); t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        r.addView(t, new LinearLayout.LayoutParams((int) (92 * getResources().getDisplayMetrics().density), LinearLayout.LayoutParams.WRAP_CONTENT));
        r.addView(field, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        return r;
    }
    /** The long descriptions that used to sit under every option; the button does not close the boot dialog. */
    private static final String HELP_TEXT =
            "USB 모드: A 없음(기존 복제본 부팅) / B USB 장치 포트만 / C 포트 + 초기 열거(일반 VM과 같은 조건) / D C + 복구 환경변수 5개 읽기 전용 조회(getenv) / E D + auto-boot를 true로 저장(clone-001 전용, 값을 바꿈)\n\n"
            + "실행 시간: 3분(GPU 시험용), 15분, 30분, 1시간, 제한 없음(중지 버튼으로 끝냄)\n\n"
            + "GPU 실제 실행: Render/Blit 명령을 Vulkan으로 실행(LAGFX_ARM64_EXEC). 끄면 해석만 함.\n\n"
            + "화면 되쓰기(GPU 실행을 켰을 때만 의미 있음, LAGFX_ARM64_MAPPED_WRITEBACK): 끔 = flag 4·동기화 때만 / 즉시 = 그린 exec마다 되쓰기(=1) / 지연 = 디스플레이 명령 직전에 한 번(=swap, 실험)\n\n"
            + "화면 시험 패턴: 게스트 swap마다 색 막대를 surface에 씀. GPU 렌더링 없이 화면 전달 경로만 확인.\n\n"
            + "디스플레이 이벤트(VBL): VBL ~60Hz와 디스플레이 원인 비트를 전달(HV_PVG_DISPLAY_EVENTS). 게스트가 enable 비트를 켰을 때만. 45초 정체 가설 시험.\n\n"
            + "셰이더·파이프라인 저장: 게스트 셰이더·파이프라인 데이터를 앱 외부 저장 폴더 g7_gpudump에 크기 제한으로 저장.\n\n"
            + "그리기별 진단(G7_DRAW_DIAG, 기본 켜짐, 마지막 선택을 기억): 라이브러리가 draw별 요약을 로그에 남김(용량 제한). 켬=1, 끔=0을 항상 명시해 전달. Render Replay 캡처와 별개.\n\n"
            + "Render Replay 캡처(LAGFX_ARM64_DUMP_DATA=1, 기본 꺼짐, 마지막 선택을 기억): draw의 버퍼·텍스처·어태치먼트 전후 이미지 저장(최대 256 draw·128 MiB). 셰이더 저장과 GPU 실행을 함께 켜야 하며 메모리 여유가 적으니 주의.\n\n"
            + "동의: 선택한 복제본의 디스크·AUX에 부팅 중 쓰기가 일어나는 것에 동의(원본은 쓰지 않음).";
    private void wireHelp(AlertDialog dlg) {
        dlg.setOnShowListener(x -> dlg.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> new AlertDialog.Builder(this).setTitle("옵션 설명").setMessage(HELP_TEXT).setPositiveButton("닫기", null).show()));
    }

    /** Starts the foreground service for a run that just started and writes the outcome of the request into that run's log. */
    private void startRunService() {
        boolean ok = G7RunService.start(this);
        G7CloneBoot.Run r = G7CloneBoot.current();
        if (r != null) r.note("service start requested, accepted by the system=" + ok);
        if (!ok) status.setText(status.getText() + "\n(포그라운드 서비스 시작 요청이 거부됐습니다. 실행은 보호 없이 계속됩니다.)");
    }

    /** The clone-boot status box: the live run's monitor, or (no run object, e.g. after a restart) the newest saved clone log. Keeps the scroll at the end when the reader was at the end. */
    private void showCloneBox(G7CloneBoot.Run r) {
        String text;
        if (r != null) text = r.monitor().render(System.currentTimeMillis(), !r.finished(), r.finished() ? r.exitCode() : 0, r.stageSummary(), r.finished() ? r.verdict() : null, r.frameNote());
        else { text = G7CloneBoot.describeSaved(getFilesDir()); if (text.isEmpty()) text = "복제본 부팅 로그가 아직 없습니다."; }
        View c = cbScroll.getChildAt(0);
        boolean atEnd = cbScroll.getScrollY() + cbScroll.getHeight() >= c.getHeight() - 40;
        cbMon.setText(text);
        if (atEnd) cbScroll.post(() -> cbScroll.fullScroll(View.FOCUS_DOWN));
    }

    /** The guest asked for a restart and the policy allowed it: the same clone again, through the same gates as a person's start (validate, isolation, ROM/ECID). */
    private void restartCloneBoot(int gen) {
        if (gen != restartGen || !restartPending || restartClone == null) return;   // cancelled, or a newer start/restart replaced this one
        restartPending = false;
        G7CloneBoot.Start st = G7CloneBoot.start(nativeDirRef, extDirRef, privDir, getFilesDir(), restartClone, true, runnerSt(), restartUsb, restartSeconds);
        status.setText("자동 재시작: " + st.message);
        if (st.started) { cbtn.setEnabled(false); cbStop.setEnabled(true); startRunService(); cbootPoll(); }
        else { restartClone = null; cbtn.setEnabled(true); cbStop.setEnabled(false); }   // the restart did not start: the person can start by hand again
    }

    private void cbootPoll() {
        final G7CloneBoot.Run r = G7CloneBoot.current();
        if (r == null) return;
        showCloneBox(r);
        G7CloneBoot.Frame f = r.frame();
        if (f != null && f != cbShownFrame) {   // a new picture: drawn from memory (never written), scaled up without smoothing so single pixels stay visible
            int[] px = new int[f.w * f.h];
            for (int i = 0; i < px.length; i++) px[i] = 0xff000000 | ((f.rgb[3 * i] & 0xff) << 16) | ((f.rgb[3 * i + 1] & 0xff) << 8) | (f.rgb[3 * i + 2] & 0xff);
            android.graphics.Bitmap bm = android.graphics.Bitmap.createBitmap(px, f.w, f.h, android.graphics.Bitmap.Config.ARGB_8888);
            android.graphics.drawable.BitmapDrawable bd = new android.graphics.drawable.BitmapDrawable(getResources(), bm); bd.setFilterBitmap(false);
            cbImage.setImageDrawable(bd); cbImage.setMinimumHeight((int) (getResources().getDisplayMetrics().density * 200));
            cbImageNote.setText("화면 사본 " + f.w + "x" + f.h + ", 통계 분류(추정): " + f.cls + ", 검지 않은 화소 " + f.nonblackPct + "%, 색 " + f.colors + "종. 분류는 Apple 로고인지 부팅 화면인지 구분하지 못합니다. 이미지를 직접 보고 판단하세요.");
            cbShownFrame = f;
        }
        if (f == null) cbImageNote.setText(r.frameNote() + (r.frameAnsweredMs == 0 ? "" : " (응답 " + (System.currentTimeMillis() - r.frameAnsweredMs) / 1000 + "초 전)"));
        if (r.finished()) {
            cbtn.setEnabled(true); cbStop.setEnabled(false);
            String msg = "복제본 부팅이 끝났습니다(종료 코드 " + r.exitCode() + "). " + r.verdict();
            if (restartClone != null && !restartPending) {
                G7RestartPolicy.Decision d = restartPolicy.decide(r.parsedVerdict(), r.wasCancelled() || r.exitCode() != 0, System.currentTimeMillis());
                msg += "\n" + d.reason;
                if (d.restart) {
                    restartPending = true; final int gen = ++restartGen;
                    cbtn.setEnabled(false); cbStop.setEnabled(true);   // while waiting: no second start, and the stop button cancels the restart
                    log.postDelayed(() -> restartCloneBoot(gen), d.delayMs);
                } else { restartClone = null; G7RunService.stop(this); }
            }
            status.setText(msg);
            return;
        }
        log.postDelayed(this::cbootPoll, 2000);
    }

    private void runClone() {
        setDeleteButtons(false); cloneBtn.setEnabled(false); cloneFromBtn.setEnabled(false); cancel.setEnabled(true);
        final G7CloneProgress pg = G7CloneProgress.begin("설치 쌍", System.currentTimeMillis()); showCloneProgress();
        new Thread(() -> {
            String m; boolean good = false;
            try { G7Clone.Result r = G7Clone.clone(privDir, runnerSt(), EXTENTS, SIZER, privDir::getUsableSpace, () -> cloneCancel, pg); m = r.message; good = r.ok; }
            catch (RuntimeException e) { m = "복제 중 예기치 못한 오류가 났습니다. 원본은 바뀌지 않았습니다."; }
            pg.finish(good, m);
            final String msg = m;
            runOnUiThread(() -> { refreshStorage(); showSummary(); setDeleteButtons(true); cloneBtn.setEnabled(true); cloneFromBtn.setEnabled(true); cancel.setEnabled(false); status.setText(msg); });
        }, "g7-clone").start();
    }

    /** Pick ONE clone folder (name and the space it frees), confirm, delete. The install pair and the other clones are not touched; a live boot or copy blocks it (checked again inside G7Storage). */
    private void confirmDeleteClone() {
        final List<G7Storage.Item> items = G7Storage.listClones(privDir, SIZER);
        if (items.isEmpty()) { status.setText("삭제할 복제본이 없습니다."); return; }
        final String[] names = new String[items.size()];
        for (int i = 0; i < names.length; i++) names[i] = items.get(i).name + " · 확보 " + G7Storage.fmt(items.get(i).allocated);
        new AlertDialog.Builder(this).setTitle("삭제할 복제본 고르기")
                .setItems(names, (d, which) -> {
                    final G7Storage.Item it = items.get(which);
                    new AlertDialog.Builder(this).setTitle(it.name + " 영구 삭제")
                            .setMessage(it.name + "을(를) 영구 삭제하고 약 " + G7Storage.fmt(it.allocated) + "을(를) 확보합니다. 되돌릴 수 없습니다. 설치 쌍과 다른 복제본은 지우지 않습니다."
                                    + (it.name.equals("clone-001") ? "\n\n주의: clone-001은 새 복제본을 만드는 기준(원본)으로 쓰던 복제본입니다. 지우면 다시 설치 쌍에서 복제해야 합니다." : "")
                                    + "\n복제본 부팅이나 복제가 진행 중이면 삭제되지 않습니다.")
                            .setNegativeButton("취소", null)
                            .setPositiveButton("삭제", (dd, ww) -> runDelete(() -> G7Storage.deleteClone(privDir, it.name, runnerSt(),
                                    () -> G7CloneBoot.hasLive() || G7CloneProgress.current().running).message)).show();
                })
                .setNegativeButton("취소", null).show();
    }

    private void confirmDeleteCurrent() {
        new AlertDialog.Builder(this).setTitle("영구 삭제 확인")
                .setMessage("지금 vm/에 있는 실패한 설치 쌍(디스크·AUX·AUX 원본 복사·상태 파일)을 격리 없이 바로 영구 삭제합니다. 상태가 in_progress인 쌍만 지우며, 정상 설치가 끝난(done) 쌍은 지우지 않습니다. 되돌릴 수 없습니다.")
                .setNegativeButton("취소", null)
                .setPositiveButton("삭제", (d, w) -> runDelete(() -> G7Storage.deleteCurrent(privDir, runnerSt()).message)).show();
    }

    /** Deleting a large file can take a moment: off the screen thread; the buttons stay off until it ends. */
    private void runDelete(final java.util.function.Supplier<String> job) {
        setDeleteButtons(false);
        status.setText("삭제 중입니다…");
        new Thread(() -> {
            String m;
            try { m = job.get(); } catch (RuntimeException e) { m = "삭제 중 예기치 못한 오류가 났습니다. 남은 파일은 그대로이며 다시 시도할 수 있습니다."; }
            final String msg = m;
            runOnUiThread(() -> {
                refreshStorage(); showSummary(); setDeleteButtons(true);
                status.setText(msg + "\n\n" + G7ExecLauncher.preflight(nativeDirRef, extDirRef, privDir).text);
            });
        }, "g7-delete").start();
    }

    private void poll() {
        log.postDelayed(() -> {
            G7ExecLauncher.Run r = G7ExecLauncher.current();
            if (r == null) return;
            log.setText(r.tail());
            showSummary();
            if (r.finished()) { status.setText(G7ExecLauncher.meaning(r.exitCode())); cancel.setEnabled(false); } else poll();
        }, 2000);
    }

    private void netPoll() {
        log.postDelayed(() -> {
            G7NetProbe.Run r = G7NetProbe.current();
            if (r == null) return;
            log.setText(r.tail());
            if (r.finished()) { status.setText("네트워크 시험이 끝났습니다(종료 코드 " + r.exitCode() + (r.cancelled() ? ", 중지됨" : "") + "). '네트워크 시험 로그 공유'로 보내세요."); netStop.setEnabled(false); } else netPoll();
        }, 2000);
    }

    private void showSummary() {
        G7ExecLauncher.setAppInfo(android.os.Process.myPid(), System.currentTimeMillis() - (android.os.SystemClock.elapsedRealtime() - android.os.Process.getStartElapsedRealtime()));
        final String base = G7ExecLauncher.summary(getFilesDir(), new File(getFilesDir(), "vm")) + "\n" + G7ExecLauncher.runnerCheck(android.os.Process.myPid(), getFilesDir());
        summary.setText(base + "\n(시스템 종료 기록을 읽는 중…)");
        showExitInfoAsync(base);
    }

    /** The reason name from the SDK's own constants (the numbers are only a fallback inside G7ExitInfo). */
    private static String sdkReasonName(int r) {
        switch (r) {
            case android.app.ApplicationExitInfo.REASON_UNKNOWN: return "UNKNOWN";
            case android.app.ApplicationExitInfo.REASON_EXIT_SELF: return "EXIT_SELF";
            case android.app.ApplicationExitInfo.REASON_SIGNALED: return "SIGNALED";
            case android.app.ApplicationExitInfo.REASON_LOW_MEMORY: return "LOW_MEMORY";
            case android.app.ApplicationExitInfo.REASON_CRASH: return "CRASH";
            case android.app.ApplicationExitInfo.REASON_CRASH_NATIVE: return "CRASH_NATIVE";
            case android.app.ApplicationExitInfo.REASON_ANR: return "ANR";
            case android.app.ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "INITIALIZATION_FAILURE";
            case android.app.ApplicationExitInfo.REASON_PERMISSION_CHANGE: return "PERMISSION_CHANGE";
            case android.app.ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "EXCESSIVE_RESOURCE_USAGE";
            case android.app.ApplicationExitInfo.REASON_USER_REQUESTED: return "USER_REQUESTED";
            case android.app.ApplicationExitInfo.REASON_USER_STOPPED: return "USER_STOPPED";
            case android.app.ApplicationExitInfo.REASON_DEPENDENCY_DIED: return "DEPENDENCY_DIED";
            case android.app.ApplicationExitInfo.REASON_OTHER: return "OTHER";
            case android.app.ApplicationExitInfo.REASON_FREEZER: return "FREEZER";
            case android.app.ApplicationExitInfo.REASON_PACKAGE_STATE_CHANGE: return "PACKAGE_STATE_CHANGE";
            case android.app.ApplicationExitInfo.REASON_PACKAGE_UPDATED: return "PACKAGE_UPDATED";
            default: return null;
        }
    }

    /** One line about the trace of a record: an ANR trace is text (at most 4 KiB counted, first line shown); a native crash trace is a protobuf tombstone, which is kept as a file (at most 1 MiB, once per record) and never shown as text. Runs off the UI thread. */
    private String traceNote(android.app.ApplicationExitInfo e) {
        try (java.io.InputStream in = e.getTraceInputStream()) {
            if (in == null) return null;
            if (e.getReason() == android.app.ApplicationExitInfo.REASON_CRASH_NATIVE) {
                String name = "tombstone_" + e.getPid() + "_" + e.getTimestamp() + ".pb";
                G7ExitTrace.Result k = G7ExitTrace.keep(in, new File(getFilesDir(), "g7exitinfo"), name);
                switch (k.status) {
                    case ALREADY_THERE: return "네이티브 충돌 tombstone(protobuf)은 이미 보존됨: 앱 파일 g7exitinfo/" + name;
                    case LIMIT_REACHED: return "네이티브 충돌 tombstone(protobuf): 보관 한도(" + G7ExitTrace.MAX_FILES + "개/" + (G7ExitTrace.MAX_TOTAL_BYTES >> 20) + "MiB)에 도달해 저장하지 않음";
                    default: return "네이티브 충돌 tombstone(protobuf) " + k.bytes + "바이트 보존" + (k.status == G7ExitTrace.Status.KEPT_TRUNCATED ? " — 잘린 기록(원본이 더 큼)" : "") + ": 앱 파일 g7exitinfo/" + name;
                }
            }
            byte[] buf = new byte[4096]; int n = in.read(buf);
            if (n <= 0) return null;
            String first = new String(buf, 0, n, java.nio.charset.StandardCharsets.UTF_8); int nl = first.indexOf('\n'); if (nl > 0) first = first.substring(0, nl);
            return "ANR 텍스트 첫 줄: " + (first.length() > 120 ? first.substring(0, 120) : first);
        } catch (Throwable t) { return "trace 읽기 실패: " + t.getClass().getSimpleName(); }
    }

    /** Reads this package's historical process exit records (API 30+; reading only) on a worker thread and appends them to the summary when ready. */
    private void showExitInfoAsync(final String base) {
        new Thread(() -> {
            String text;
            try {
                android.app.ActivityManager am = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
                java.util.List<android.app.ApplicationExitInfo> in = am.getHistoricalProcessExitReasons(getPackageName(), 0, 10);
                java.util.List<G7ExitInfo.Record> out = new java.util.ArrayList<>();
                for (android.app.ApplicationExitInfo e : in) out.add(new G7ExitInfo.Record(e.getPid(), e.getProcessName(), e.getTimestamp(), e.getReason(), e.getStatus(), e.getDescription(), e.getPss(), e.getRss(), sdkReasonName(e.getReason()), traceNote(e)));
                text = G7ExitInfo.format(out, android.os.Process.myPid(), 10);
            } catch (Throwable t) { text = "시스템 종료 기록을 읽지 못했습니다: " + t.getClass().getSimpleName() + "\n"; }
            final String t2 = text;
            runOnUiThread(() -> summary.setText(base + "\n" + t2));
        }, "g7-exitinfo").start();
    }


    /** Sends the whole newest run log (read-only, through G7LogProvider and the share sheet). */
    private void shareLog() { shareFile(G7ExecLauncher.newestLogFile(getFilesDir()), "G7 로그 공유"); }

    @Override protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        if (pendingExport != null) out.putString("pendingExport", pendingExport.getName());
    }

    private File dumpRoot() { File xd = getExternalFilesDir(null); return new File(xd != null ? xd : getFilesDir(), "g7_gpudump"); }

    /** GPU pipeline reproduction without a VM (G7PipeRepro): picks a pipefail_* bundle saved by a GPU-dump run, rebuilds that pipeline on this phone's Vulkan device, shows and saves the report. No clone, ROM or disk is touched. */
    private void choosePipeRepro() {
        if (G7CloneBoot.hasLive()) { status.setText("복제본 부팅이 실행 중입니다. 끝난 뒤에 재현 시험을 하세요(GPU를 함께 쓰면 결과를 해석하기 어렵습니다)."); return; }
        if (!G7PipeRepro.isPackaged(nativeDirRef)) { status.setText("이 APK에는 재현 실행 파일(" + G7PipeRepro.LIB_NAME + ")이 없습니다."); return; }
        final List<File> bundles = G7PipeRepro.listBundles(dumpRoot());
        if (bundles.isEmpty()) { status.setText("재현할 번들이 없습니다. 부팅 대화상자에서 '셰이더·파이프라인 저장'을 켜고 실행하면, 파이프라인 생성이 실패한 경우에만 " + dumpRoot() + "/run-*/pipefail_* 폴더가 생깁니다."); return; }
        final String[] names = new String[bundles.size()];
        for (int i = 0; i < names.length; i++) names[i] = bundles.get(i).getParentFile().getName() + " / " + bundles.get(i).getName();
        final int[] pick = { 0 };
        final android.widget.CheckBox mx = new android.widget.CheckBox(this); mx.setText("변형 시험(Matrix) 포함: 진단 전용, 각 변형은 원본에서 따로 시작, 정상 실행에는 적용되지 않음");
        new android.app.AlertDialog.Builder(this).setTitle("재현할 실패 번들").setSingleChoiceItems(names, 0, (d, w) -> pick[0] = w).setView(mx)
            .setNegativeButton("취소", null).setPositiveButton("재현 시작", (d, w) -> {
                final File b = bundles.get(pick[0]); final boolean matrix = mx.isChecked();
                status.setText("재현 중: " + b.getName() + (matrix ? " (Matrix 포함)" : "") + " …(최대 3분)");
                new Thread(() -> {
                    final G7PipeRepro.Result r = G7PipeRepro.run(nativeDirRef, b, matrix, getFilesDir());
                    runOnUiThread(() -> {
                        status.setText("재현 종료(코드 " + r.exitCode + ")" + (r.file != null ? ", 저장: " + r.file.getName() : ""));
                        android.widget.TextView tv = new android.widget.TextView(this); tv.setText(r.text); tv.setTextIsSelectable(true); tv.setTextSize(11); tv.setTypeface(android.graphics.Typeface.MONOSPACE);
                        ScrollView sv = new ScrollView(this); sv.addView(tv);
                        new android.app.AlertDialog.Builder(this).setTitle("GPU 파이프라인 재현 결과").setView(sv).setPositiveButton("닫기", null)
                            .setNeutralButton("복사", (dd, ww) -> { android.content.ClipboardManager cm = (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE); if (cm != null) cm.setPrimaryClip(android.content.ClipData.newPlainText("pipelinerepro", r.text)); }).show();
                    });
                }, "g7-pipe-repro").start();
            }).show();
    }

    /** Lists the dump runs (newest first, the newest preselected) and builds the ZIP of the chosen one: its dump files plus the VM log and the lagfx log of that run. Only after the run has ended; nothing is deleted. */
    private void chooseDumpExport() {
        if (G7CloneBoot.hasLive()) { status.setText("복제본 부팅이 실행 중입니다. 실행이 끝난 뒤에 내보낼 수 있습니다(로그와 덤프가 아직 쓰이는 중)."); return; }
        final List<G7ExecDumpExport.DumpRun> runs = G7ExecDumpExport.listRuns(dumpRoot());
        if (runs.isEmpty()) { status.setText("내보낼 덤프 실행이 없습니다(" + dumpRoot() + " 에 run-* 폴더 없음). '게스트 셰이더·파이프라인 데이터 저장'을 켜고 부팅한 뒤 다시 시도하세요."); return; }
        final String[] names = new String[runs.size()];
        for (int i = 0; i < names.length; i++) names[i] = (i == 0 ? "[최신] " : "") + runs.get(i).label();
        final int[] pick = {0};
        new AlertDialog.Builder(this).setTitle("내보낼 실행 선택").setSingleChoiceItems(names, 0, (d, w) -> pick[0] = w)
                .setNegativeButton("취소", null)
                .setNeutralButton("파일로 저장", (d, w) -> buildExport(runs.get(pick[0]), true))
                .setPositiveButton("공유", (d, w) -> buildExport(runs.get(pick[0]), false)).show();
    }

    private void buildExport(final G7ExecDumpExport.DumpRun dr, final boolean save) {
        // the dialog may have stayed open while a boot started: a run in progress is still writing its logs and dump, so re-check right now, not only when the dialog opened
        if (G7CloneBoot.hasLive() || G7CloneProgress.current().running) { status.setText("복제본 부팅이 실행 중입니다. 실행이 끝난 뒤에 내보낼 수 있습니다."); return; }
        status.setText("ZIP을 만드는 중… (" + dr.id + ")");
        final String ver = android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL + ", SDK " + android.os.Build.VERSION.SDK_INT;
        new Thread(() -> {
            String msg; File zip = null;
            try {
                if (G7CloneBoot.hasLive()) throw new IOException("a boot started while the export was being prepared");
                G7ExecDumpExport.Result r = G7ExecDumpExport.export(getFilesDir(), dr, getPackageName() + " " + BuildConfig.G7_BUILD_KIND, G7ExecLauncher.HENGE_SHA, G7ExecLauncher.LAGFX_SHA, ver);
                zip = r.zip; msg = "내보내기 ZIP 완료: " + r.message;
            } catch (IOException | RuntimeException e) { msg = "내보내기 실패: " + e.getClass().getSimpleName() + " " + e.getMessage(); }
            final String m = msg; final File z = zip;
            runOnUiThread(() -> {
                status.setText(m);
                if (z == null) return;
                if (!save) { try { shareZip(z); } catch (RuntimeException e) { status.setText(m + " / 공유 실패: " + e.getClass().getSimpleName()); } return; }
                pendingExport = z;
                Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("application/zip"); i.putExtra(Intent.EXTRA_TITLE, z.getName());
                try { startActivityForResult(i, REQ_SAVE_ZIP); } catch (RuntimeException e) { status.setText(m + " / 저장 창을 열 수 없습니다: " + e.getClass().getSimpleName()); }
            });
        }, "g7-dump-export").start();
    }

    private void shareZip(File z) {
        Uri u = Uri.parse("content://" + G7LogProvider.AUTHORITY + "/" + z.getName());
        Intent i = new Intent(Intent.ACTION_SEND);
        i.setType("application/zip");
        i.putExtra(Intent.EXTRA_STREAM, u);
        i.setClipData(ClipData.newRawUri("g7 export", u));
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivity(Intent.createChooser(i, "GPU 덤프 ZIP 공유"));
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != REQ_SAVE_ZIP) return;
        final File z = pendingExport; pendingExport = null;
        if (res != RESULT_OK || data == null || data.getData() == null || z == null) { status.setText("저장을 취소했습니다(ZIP은 앱 안에 그대로 있습니다: " + (z == null ? "-" : z.getName()) + ")."); return; }
        final Uri dest = data.getData();
        new Thread(() -> {
            String msg;
            try { G7ExecDumpExport.copyTo(z, getContentResolver().openOutputStream(dest)); msg = "저장 완료: " + z.getName(); }
            catch (IOException | RuntimeException e) { msg = "저장 실패: " + e.getClass().getSimpleName() + " " + e.getMessage(); }
            final String m = msg; runOnUiThread(() -> status.setText(m));
        }, "g7-dump-save").start();
    }

    private void shareFile(File f, String title) {
        if (f == null) { status.setText("공유할 로그가 없습니다."); return; }
        Uri u = Uri.parse("content://" + G7LogProvider.AUTHORITY + "/" + f.getName());
        Intent i = new Intent(Intent.ACTION_SEND);
        i.setType("text/plain");
        i.putExtra(Intent.EXTRA_STREAM, u);
        i.setClipData(ClipData.newRawUri("g7 log", u));
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivity(Intent.createChooser(i, title));
    }

    private void add(LinearLayout root, String s, int sp) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setPadding(0, 0, 0, 24);
        root.addView(t);
    }
}
