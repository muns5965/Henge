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

    private static final int[] SIZES_MIB = {1, 16, 64, 256, 768};

    private TextView output;
    private Button runButton;
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
        return "기기: " + Build.MANUFACTURER + " " + Build.MODEL + "\n"
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

    private void copyResult() {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        cm.setPrimaryClip(ClipData.newPlainText("henge-jit-probe", output.getText()));
        Toast.makeText(this, "복사했습니다", Toast.LENGTH_SHORT).show();
    }
}
