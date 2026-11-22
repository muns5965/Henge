package dev.muns.hengeprobe;

import android.app.Activity;
import android.os.Bundle;
import android.util.TypedValue;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;

/**
 * [A-INST] Readiness screen for the installed-disk boot path. A diagnostic, not a boot button: it starts nothing --
 * no button, no automatic start, no Intent extras read, no SHM/JNI -- and (checked by
 * tools/check-installed-launcher-source.sh) never names the app's OTHER runner launchers or real-VM screens, so
 * opening it can never start one of those instead. All it does is render VmInstalledDiag.report() and the launcher's
 * own switch state.
 */
public class VmRunnerInstalledActivity extends Activity {
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int p = (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 16, getResources().getDisplayMetrics());
        root.setPadding(p, p, p, p);
        line(root, "[A-INST] 설치 디스크 부팅 준비 상태", 18);
        line(root, VmInstalledDiag.report(new File(getExternalFilesDir(null), "vm"), new File(getFilesDir(), "vm"),
                new File(getApplicationInfo().nativeLibraryDir)), 13);
        if (!VmRunnerInstalledLauncher.startEnabled()) line(root, VmRunnerInstalledLauncher.REFUSAL, 12);
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
