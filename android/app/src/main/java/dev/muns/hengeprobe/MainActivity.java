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

/** One screen: run the JIT mapping probe and show the result as text. */
public class MainActivity extends Activity {
    static {
        System.loadLibrary("henge_probe");
    }

    private native String runProbe(String crashPath);

    private TextView output;
    private File crashFile;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        crashFile = new File(getFilesDir(), "last_crash.txt");

        int pad = (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 16, getResources().getDisplayMetrics());
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad * 2, pad, pad);

        Button run = new Button(this);
        run.setText("JIT 테스트 실행");
        run.setOnClickListener(v -> runNow());
        root.addView(run);

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
        String crash = readCrash();
        if (crash != null) {
            sb.append("\n이전 실행이 비정상 종료됨:\n").append(crash);
            //noinspection ResultOfMethodCallIgnored
            crashFile.delete();
        }
        sb.append("\n'JIT 테스트 실행'을 누르세요.\n");
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

    private String readCrash() {
        if (!crashFile.exists() || crashFile.length() == 0) return null;
        try (FileInputStream in = new FileInputStream(crashFile)) {
            byte[] data = new byte[(int) Math.min(crashFile.length(), 4096)];
            int n = in.read(data);
            return new String(data, 0, Math.max(n, 0), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return null;
        }
    }

    private void runNow() {
        String result = runProbe(crashFile.getAbsolutePath());
        output.setText(deviceInfo() + "\n" + result);
    }

    private void copyResult() {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        cm.setPrimaryClip(ClipData.newPlainText("henge-jit-probe", output.getText()));
        Toast.makeText(this, "복사했습니다", Toast.LENGTH_SHORT).show();
    }
}
