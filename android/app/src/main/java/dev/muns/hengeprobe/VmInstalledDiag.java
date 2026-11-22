package dev.muns.hengeprobe;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** Read-only readiness report for the installed-disk boot path ([A-INST]). It only looks at files; it never creates,
 *  opens for writing, or starts anything, and never falls back to a blank image. It cannot tell whether macOS is
 *  installed on a disk -- a file named disk.img can equally be blank or a half-finished restore -- and says so.
 *  No Android classes: tests/java/VmInstalledDiagTest.java runs on a plain JVM. */
final class VmInstalledDiag {
    private VmInstalledDiag() {}

    private static String file(File d, String name) {
        File f = new File(d, name);
        if (!f.exists()) return name + ": 없음";
        if (!f.isFile()) return name + ": 일반 파일이 아님";
        return name + ": 있음, " + f.length() + " 바이트";
    }

    /** Reads vm_ecid.txt without creating it. */
    static String ecidLine(File privVmDir) {
        File f = new File(privVmDir, VmEcid.FILE);
        if (!f.isFile()) return "ECID: " + VmEcid.FILE + " 없음(이 화면은 만들지 않습니다)";
        try {
            String t = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).trim();
            long v = t.startsWith("ecid=") ? VmRunnerInstalledLauncher.parseHex(t.substring(5)) : -1;
            if (v < 0 || !VmEcid.inRange(v)) return "ECID: " + VmEcid.FILE + " 내용이 올바르지 않음";
            return "ECID: 앱이 보관한 값이 있음(이 값이 디스크를 설치할 때 쓴 값과 같은지는 확인할 수 없음)";
        } catch (java.io.IOException e) {
            return "ECID: 읽기 실패";
        }
    }

    static String report(File extVmDir, File privVmDir, File nativeLibDir) {
        StringBuilder s = new StringBuilder();
        s.append("실행기(").append(VmRunnerInstalledLauncher.LIB_NAME).append(") APK에 포함: ")
         .append(VmRunnerInstalledLauncher.isPackaged(nativeLibDir) ? "예" : "아니오").append('\n');
        s.append("실행: ").append(VmRunnerInstalledLauncher.startEnabled() ? "활성" : "비활성(이 빌드의 기본 상태)").append('\n');
        s.append("외부 폴더 vm/ 의 파일 (blank 이미지로 대체하지 않음)\n");
        s.append("  ").append(file(extVmDir, "rom.bin")).append('\n');
        s.append("  ").append(file(extVmDir, "disk.img")).append('\n');
        s.append("  ").append(file(extVmDir, "aux.img")).append('\n');
        s.append(ecidLine(privVmDir)).append('\n');
        s.append("macOS 설치 완료 여부: 확인할 수 없음 — 파일이 있다는 사실만으로는 설치된 디스크인지, 비어 있거나 복원이 중단된 이미지인지 구분되지 않습니다.\n");
        s.append("이 화면은 아무 프로세스도 시작하지 않았고, 파일을 만들거나 쓰지 않았습니다.");
        return s.toString();
    }
}
