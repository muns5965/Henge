package dev.muns.hengeprobe;

import android.app.Activity;
import android.os.Bundle;
import android.util.TypedValue;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * [G4 UI] Information screen for the G4 UI diagnostic runner. Mirrors the existing [C1-3] information screen
 * exactly on purpose: it starts nothing -- no button here, no automatic start, no Intent extras read, no
 * SHM/JNI, and (checked by tools/check-g4ui-launcher-source.sh) no reference anywhere in this file to the
 * app's OTHER, unrelated runner launcher or its own real-VM screen ([A]) at all, so opening this screen can
 * never, even by accident, start that other runner instead.
 *
 * Starting a process is VmRunnerG4Launcher's job alone, and that switch has no "observation only" mode (see
 * that class's own header comment: running the G4 UI binary always makes a real Apple TSS request and drives
 * the guest through DFU->Recovery->RecoveryOS the moment it starts, whichever way the switch currently reads).
 * This screen only reports what VmRunnerG4Launcher.startEnabled() currently says and whether the library made
 * it into this APK -- never why a run did or did not succeed, since no run is ever attempted here. The text
 * below renders either state ("활성"/"비활성") from that live call, never a hardcoded assumption about which
 * one this build currently is -- the switch has already changed once, by a separate, explicit approval.
 */
public class VmRunnerG4Activity extends Activity {
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int p = (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 16, getResources().getDisplayMetrics());
        root.setPadding(p, p, p, p);

        boolean packaged = VmRunnerG4Launcher.isPackaged(new java.io.File(getApplicationInfo().nativeLibraryDir));
        boolean enabled = VmRunnerG4Launcher.startEnabled();
        line(root, "[G4 UI] G4 복원 진단 실행기 (" + VmRunnerG4Launcher.LIB_NAME + ")", 18);
        line(root, "APK에 포함: " + (packaged ? "예" : "아니오(설치된 경로에서 찾지 못함)"), 14);
        line(root, "G4 실행: " + (enabled ? "활성" : "이 빌드에서는 비활성(실패가 아니라 이 빌드의 기본 상태입니다)")
                + (enabled ? "" : "\n" + VmRunnerG4Launcher.REFUSAL), 14);
        line(root, "이 화면은 지금 아무 프로세스도 시작하지 않았습니다. Apple 서버로 어떤 요청도 보내지 않았고, 게스트 상태를 바꾸지 않았습니다. "
                + "이 바이너리를 실제로 실행하면 그 순간 실제 Apple TSS 요청을 보내고 DFU→Recovery→RecoveryOS까지 진행합니다"
                + "(관찰 전용 모드가 없어서, 실행이 켜지면 언제나 그렇습니다). "
                + (enabled ? "이 빌드에서는 실행이 이미 켜져 있으므로, 실제로 시작하는 다른 화면(예: [A-G4])을 열면 그 즉시 이 일이 일어납니다."
                           : "실행을 켜는 것은 이 화면과는 별개의, 그때마다 새로 받는 승인이 필요한 변경입니다."), 12);

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
