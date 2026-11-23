package dev.muns.hengeprobe;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;

/**
 * [G7-EXEC] Only in the g7exec build (never in the general APK). Shows what is there (files only: the binary is NOT executed to find out), and starts the G7 execution build
 * only through G7ExecLauncher, which refuses while the build's start switch is off (g7exec) and otherwise needs the person's consent (a checked box; nothing is typed) and the built-in IPSW SHA-256, which the runner verifies against the stored file.
 * No Intent extras are read. The ECID is never shown.
 */
public class G7ExecActivity extends Activity {
    private TextView status, log, summary;
    private CheckBox consent, isoConsent;
    private Button start, cancel, refresh, share, copy, isolate;
    // No run state lives in this Activity: G7ExecLauncher.current() is the one source, so a recreated screen (rotation, back and forth) sees and can cancel the same process.

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        final File ext = new File(getExternalFilesDir(null), "vm"), priv = new File(getFilesDir(), "vm");
        final File nativeDir = new File(getApplicationInfo().nativeLibraryDir);
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
        });
        share = new Button(this); share.setText("로그 파일 공유(전체)"); root.addView(share);
        copy = new Button(this); copy.setText("로그 끝부분 복사"); root.addView(copy);
        log = new TextView(this);
        log.setTextIsSelectable(true);
        log.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        log.setTypeface(android.graphics.Typeface.MONOSPACE);
        log.setText(G7ExecLauncher.readLogTail(getFilesDir(), 6000));
        root.addView(log);
        showSummary();
        String note = G7ExecLauncher.lastRunNote(getFilesDir());
        if (!note.isEmpty()) status.setText(status.getText() + "\n\n" + note);
        G7ExecLauncher.Run live = G7ExecLauncher.current();
        if (live != null && !live.finished()) { start.setEnabled(false); cancel.setEnabled(true); poll(); }
        start.setOnClickListener(v -> {
            G7ExecLauncher.Start s = G7ExecLauncher.start(nativeDir, ext, priv, getFilesDir(), consent.isChecked());
            status.setText(s.message);
            consent.setChecked(false);   // every start needs a fresh consent
            if (s.started) { start.setEnabled(false); cancel.setEnabled(true); poll(); }
        });
        cancel.setOnClickListener(v -> { G7ExecLauncher.Run r = G7ExecLauncher.current(); if (r != null) { r.cancel(); status.setText("취소를 요청했습니다(복원을 멈추고 쌍은 in_progress로 보존)."); } });
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

    private void poll() {
        log.postDelayed(() -> {
            G7ExecLauncher.Run r = G7ExecLauncher.current();
            if (r == null) return;
            log.setText(r.tail());
            showSummary();
            if (r.finished()) { status.setText(G7ExecLauncher.meaning(r.exitCode())); cancel.setEnabled(false); } else poll();
        }, 2000);
    }

    private void showSummary() { summary.setText(G7ExecLauncher.summary(getFilesDir(), new File(getFilesDir(), "vm")) + "\n" + G7ExecLauncher.runnerCheck(android.os.Process.myPid(), getFilesDir())); }

    /** Sends the whole newest run log (read-only, through G7LogProvider and the share sheet). */
    private void shareLog() {
        File f = G7ExecLauncher.newestLogFile(getFilesDir());
        if (f == null) { status.setText("공유할 로그가 없습니다."); return; }
        Uri u = Uri.parse("content://" + G7LogProvider.AUTHORITY + "/" + f.getName());
        Intent i = new Intent(Intent.ACTION_SEND);
        i.setType("text/plain");
        i.putExtra(Intent.EXTRA_STREAM, u);
        i.setClipData(ClipData.newRawUri("g7 log", u));
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivity(Intent.createChooser(i, "G7 로그 공유"));
    }

    private void add(LinearLayout root, String s, int sp) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setPadding(0, 0, 0, 24);
        root.addView(t);
    }
}
