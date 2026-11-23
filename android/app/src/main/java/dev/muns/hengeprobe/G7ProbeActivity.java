package dev.muns.hengeprobe;

import android.app.Activity;
import android.os.Bundle;
import android.os.StatFs;
import android.util.TypedValue;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;

/**
 * [G7-PRE] The G7 preflight on this phone's real files. The only thing it can start is g7_probe (through G7ProbeLauncher), which reads files and exits;
 * it never starts a machine, a restore or a download, creates no disk or AUX and writes nothing. The button runs it once per tap, off the UI thread.
 * No Intent extras are read. The ECID is never shown.
 */
public class G7ProbeActivity extends Activity {
    private TextView result;
    private Button run;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int p = (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 16, getResources().getDisplayMetrics());
        root.setPadding(p, p, p, p);
        line(root, "[G7-PRE] G7 사전검사 (읽기 전용)", 18);
        File ext = new File(getExternalFilesDir(null), "vm"), priv = new File(getFilesDir(), "vm");
        File nativeDir = new File(getApplicationInfo().nativeLibraryDir);
        StringBuilder s = new StringBuilder();
        s.append("실행 파일(").append(G7ProbeLauncher.LIB_NAME).append("): ").append(G7ProbeLauncher.isPackaged(nativeDir) ? "APK에 포함됨" : "APK에 없음 (MISSING)").append('\n');
        File rom = G7ProbeLauncher.usableRomFile(ext, priv);
        s.append("사용할 ROM: ").append(rom == null ? "정상 ROM 없음" : rom.equals(new File(ext, "rom.bin")) ? "외부 vm/rom.bin" : "앱 내부 저장본").append('\n');
        s.append(VmInstalledDiag.ecidLine(priv)).append('\n');
        s.append(VmInstalledDiag.ipswLines(priv)).append('\n');
        try {
            StatFs st = new StatFs(priv.exists() ? priv.getPath() : getFilesDir().getPath());
            s.append("앱 저장공간 여유: ").append(st.getAvailableBytes()).append(" 바이트 (사전검사의 하한 32 GiB는 검증된 최소 요구량이 아니라 임의의 안전 하한입니다)\n");
        } catch (RuntimeException e) { s.append("앱 저장공간 여유: 읽지 못함\n"); }
        s.append("이 화면은 설치 디스크·AUX를 만들지 않고, 복원을 시작하지 않으며, Apple 서버에 요청하지 않습니다.");
        line(root, s.toString(), 13);
        run = new Button(this);
        run.setText("사전검사 실행");
        run.setEnabled(G7ProbeLauncher.isPackaged(nativeDir) && rom != null);
        result = new TextView(this);
        result.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        result.setTypeface(android.graphics.Typeface.MONOSPACE);
        final File fRom = rom;
        run.setOnClickListener(v -> {
            run.setEnabled(false);
            result.setText("실행 중…");
            new Thread(() -> {
                long ecid = G7ProbeLauncher.readEcid(priv);
                G7ProbeLauncher.Result r = G7ProbeLauncher.run(nativeDir, priv, priv, priv, fRom, ecid);
                final String text = r.meaning + "\n\n" + r.output;
                runOnUiThread(() -> { result.setText(text); run.setEnabled(true); });
            }).start();
        });
        root.addView(run);
        root.addView(result);
        ScrollView sv = new ScrollView(this);
        sv.addView(root);
        setContentView(sv);
    }

    private void line(LinearLayout root, String s, int sp) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setPadding(0, 0, 0, 24);
        root.addView(t);
    }
}
