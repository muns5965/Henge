package dev.muns.hengeprobe;

import android.app.Activity;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;

/**
 * [G7-EXEC] Only in the g7exec build (never in the general APK). Shows what is there (files only: the binary is NOT executed to find out), and starts the G7 execution build
 * only through G7ExecLauncher, which refuses while START_ENABLED is false (committed) and otherwise needs the exact approval phrase and the IPSW SHA-256 typed here.
 * No Intent extras are read. The ECID is never shown.
 */
public class G7ExecActivity extends Activity {
    private TextView status, log;
    private EditText sha, phrase;
    private Button start, cancel, refresh;
    private volatile G7ExecLauncher.Run run;

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
        status = new TextView(this);
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        status.setText(G7ExecLauncher.preflight(nativeDir, ext, priv).text);
        root.addView(status);
        sha = new EditText(this); sha.setHint("IPSW SHA-256 (64자리, 직접 확인한 값)"); sha.setSingleLine(true); root.addView(sha);
        phrase = new EditText(this); phrase.setHint("승인 문구: " + G7ExecLauncher.APPROVAL_PHRASE); phrase.setSingleLine(true); root.addView(phrase);
        start = new Button(this); start.setText("시작"); root.addView(start);
        cancel = new Button(this); cancel.setText("취소"); cancel.setEnabled(false); root.addView(cancel);
        refresh = new Button(this); refresh.setText("로그 새로고침"); root.addView(refresh);
        log = new TextView(this);
        log.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        log.setTypeface(android.graphics.Typeface.MONOSPACE);
        log.setText(G7ExecLauncher.readLogTail(getFilesDir(), 6000));
        root.addView(log);
        start.setOnClickListener(v -> {
            G7ExecLauncher.Start s = G7ExecLauncher.start(nativeDir, ext, priv, getFilesDir(), phrase.getText().toString(), sha.getText().toString().trim());
            status.setText(s.message);
            if (s.started) { run = s.run; start.setEnabled(false); cancel.setEnabled(true); poll(); }
        });
        cancel.setOnClickListener(v -> { G7ExecLauncher.Run r = run; if (r != null) { r.cancel(); status.setText("취소를 요청했습니다(복원을 멈추고 쌍은 in_progress로 보존)."); } });
        refresh.setOnClickListener(v -> log.setText(run != null ? run.tail() : G7ExecLauncher.readLogTail(getFilesDir(), 6000)));
        ScrollView sv = new ScrollView(this);
        sv.addView(root);
        setContentView(sv);
    }

    private void poll() {
        log.postDelayed(() -> {
            G7ExecLauncher.Run r = run;
            if (r == null) return;
            log.setText(r.tail());
            if (r.finished()) { status.setText(G7ExecLauncher.meaning(r.exitCode())); cancel.setEnabled(false); } else poll();
        }, 2000);
    }

    private void add(LinearLayout root, String s, int sp) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setPadding(0, 0, 0, 24);
        root.addView(t);
    }
}
