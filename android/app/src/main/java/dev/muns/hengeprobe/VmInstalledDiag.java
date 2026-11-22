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

    /** The macOS 15.6.1 (24G90) restore IPSW's full size, as MainActivity's download flow already states it. */
    static final long IPSW_EXPECTED_BYTES = 16814137790L;
    static final String IPSW_NAME = "UniversalMac_15.6.1_24G90_Restore.ipsw";

    /** Which ROM the app would use, decided exactly like MainActivity.findLocalRom(): the external vm/rom.bin if genuine,
     *  else the app's internal saved copy if genuine. Reads only; never downloads. */
    static String usableRom(File extVmDir, File privVmDir) { return usableRom(extVmDir, privVmDir, VmRomCheck.LENGTH, VmRomCheck.SHA256); }

    static String usableRom(File extVmDir, File privVmDir, int length, String sha256) {
        if (VmRomCheck.isGenuine(new File(extVmDir, "rom.bin"), length, sha256)) return "외부 vm/rom.bin";
        if (VmRomCheck.isGenuine(new File(privVmDir, "AVPBooter.vmapple2.bin"), length, sha256)) return "앱 내부 저장본(AVPBooter.vmapple2.bin)";
        return "없음 (이 화면은 ROM을 받지 않습니다)";
    }

    private static String sized(File f) { return f.isFile() ? "있음, " + f.length() + " 바이트" : f.exists() ? "일반 파일이 아님" : "없음"; }

    /** The restore IPSW's three possible files, by existence and size only (never opened, hashed, resumed, reset or deleted).
     *  The downloader (host/ipsw_dl.c) writes <name>.part + <name>.meta while downloading and installs <name> only after the
     *  SHA-256 matches -- so a final file is what that procedure leaves behind, but THIS screen has not checked it. */
    static String ipswLines(File privVmDir) {
        File fin = new File(privVmDir, IPSW_NAME), part = new File(privVmDir, IPSW_NAME + ".part"), meta = new File(privVmDir, IPSW_NAME + ".meta");
        StringBuilder s = new StringBuilder("복원용 IPSW (" + IPSW_NAME + ")\n");
        if (!fin.exists() && !part.exists() && !meta.exists()) return s.append("  다운로드 파일 없음").toString();
        if (fin.exists()) {
            if (!fin.isFile()) s.append("  최종 파일: 일반 파일이 아님\n");
            else if (fin.length() == IPSW_EXPECTED_BYTES)
                s.append("  최종 파일: 있음, ").append(fin.length()).append(" 바이트(기대 크기와 일치). 정상 다운로드 절차에서는 SHA-256 검증 후에만 이 이름으로 저장됨 — 이 화면은 현재 무결성을 검증하지 않음\n");
            else
                s.append("  최종 파일: 있음, ").append(fin.length()).append(" 바이트 — 기대 크기(").append(IPSW_EXPECTED_BYTES).append(")와 불일치. 파일 상태 확인 필요\n");
        } else s.append("  최종 파일: 없음\n");
        if (part.exists() || meta.exists()) {
            s.append("  부분 다운로드 .part: ").append(sized(part)).append('\n');
            s.append("  상태 기록 .meta: ").append(sized(meta)).append('\n');
            s.append("  이어받기 가능 여부는 확인하지 않음(다운로더가 .meta와 .part를 맞춰 보고 결정함)\n");
        }
        return s.substring(0, s.length() - 1);
    }

    static String report(File extVmDir, File privVmDir, File nativeLibDir) {
        StringBuilder s = new StringBuilder();
        s.append("실행기(").append(VmRunnerInstalledLauncher.LIB_NAME).append(") APK에 포함: ")
         .append(VmRunnerInstalledLauncher.isPackaged(nativeLibDir) ? "예" : "아니오").append('\n');
        s.append("실행: ").append(VmRunnerInstalledLauncher.startEnabled() ? "활성" : "비활성(이 빌드의 기본 상태)").append('\n');
        s.append("ROM (외부 정상본 우선, 없으면 앱 내부 저장본)\n");
        s.append("  외부 vm/rom.bin: ").append(VmRomCheck.info(new File(extVmDir, "rom.bin"))).append('\n');
        s.append("  내부 AVPBooter.vmapple2.bin: ").append(VmRomCheck.info(new File(privVmDir, "AVPBooter.vmapple2.bin"))).append('\n');
        s.append("  사용할 ROM: ").append(usableRom(extVmDir, privVmDir)).append('\n');
        s.append(ipswLines(privVmDir)).append('\n');
        s.append("외부 폴더 vm/ 의 설치 대상 파일 (blank 이미지로 대체하지 않음)\n");
        s.append("  ").append(file(extVmDir, "disk.img")).append('\n');
        s.append("  ").append(file(extVmDir, "aux.img")).append('\n');
        s.append(ecidLine(privVmDir)).append('\n');
        s.append("macOS 설치 완료 여부: 확인할 수 없음 — 파일이 있다는 사실만으로는 설치된 디스크인지, 비어 있거나 복원이 중단된 이미지인지 구분되지 않습니다.\n");
        s.append("이 화면은 아무 프로세스도 시작하지 않았고, 파일을 만들거나 쓰거나 받지 않았습니다.");
        return s.toString();
    }
}
