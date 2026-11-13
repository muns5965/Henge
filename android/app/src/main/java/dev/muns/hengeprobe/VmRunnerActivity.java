package dev.muns.hengeprobe;

import android.app.Activity;
import android.os.Bundle;
import android.util.TypedValue;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * [C1-3] Information screen for the VM runner. It starts nothing: no button here, no automatic start, and it does not read Intent extras.
 * Starting a process is VmRunnerLauncher's job alone, and in this build that is switched off.
 */
public class VmRunnerActivity extends Activity {
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int p = (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 16, getResources().getDisplayMetrics());
        root.setPadding(p, p, p, p);

        boolean packaged = VmRunnerLauncher.isPackaged(new java.io.File(getApplicationInfo().nativeLibraryDir));
        line(root, "[C1-3] VM 실행기 (" + VmRunnerLauncher.LIB_NAME + ")", 18);
        line(root, "APK에 포함: " + (packaged ? "예" : "아니오(설치된 경로에서 찾지 못함)"), 14);
        line(root, "VM 실행: " + (VmRunnerLauncher.startEnabled() ? "활성" : "이 빌드에서는 비활성") + "\n" + (VmRunnerLauncher.startEnabled() ? "" : VmRunnerLauncher.REFUSAL), 14);
        line(root, "실행기에 고정된 안전 설정(옵션으로 바꿀 수 없음): 저장소 읽기 전용, 네트워크 없음, 설치기 꺼짐, ECID·시드 0, HV_* 환경변수 제거. "
                + "이 화면은 아무 프로세스도 시작하지 않으며, ROM·디스크 파일을 만들거나 열지 않습니다.", 12);

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
