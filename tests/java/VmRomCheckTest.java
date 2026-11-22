package dev.muns.hengeprobe;

import java.io.File;
import java.io.FileInputStream;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Arrays;

/** Plain-java test for VmRomCheck + the [A-INST] ROM/IPSW report.
 *  javac -encoding UTF-8 -d out android/.../{VmRomCheck,RomSlice,VmInstalledDiag,VmRunnerInstalledLauncher,VmEcid}.java tests/java/VmRomCheckTest.java
 *  Compares VmRomCheck against a VERBATIM copy of MainActivity's pre-refactor isGenuineRom()/romInfo() on the same inputs. */
public class VmRomCheckTest {
    static int fails;
    static void check(String n, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + n); if (!ok) fails++; }

    // ---- verbatim copy of the old MainActivity code (constants parameterised only so synthetic files can be used) ----
    static boolean oldIsGenuine(File f, int ROM_LENGTH, String ROM_SHA256) {
        if (!f.isFile() || f.length() != ROM_LENGTH) return false;
        try (FileInputStream in = new FileInputStream(f)) {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[65536]; int n;
            while ((n = in.read(buf)) > 0) md.update(buf, 0, n);
            StringBuilder hex = new StringBuilder();
            for (byte b : md.digest()) hex.append(String.format("%02x", b));
            return hex.toString().equals(ROM_SHA256);
        } catch (Exception e) { return false; }
    }
    static String oldInfo(File f, int ROM_LENGTH, String ROM_SHA256) {
        if (!f.exists()) return "없음";
        if (!f.isFile()) return "파일이 아님";
        String sha = "?";
        try (FileInputStream in = new FileInputStream(f)) {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[65536]; int n;
            while ((n = in.read(buf)) > 0) md.update(buf, 0, n);
            StringBuilder hex = new StringBuilder();
            for (byte b : md.digest()) hex.append(String.format("%02x", b));
            sha = hex.toString();
        } catch (Exception e) { sha = "읽기 실패: " + e; }
        boolean ok = f.length() == ROM_LENGTH && sha.equals(ROM_SHA256);
        return "있음, " + f.length() + "바이트 (기대 " + ROM_LENGTH + "), SHA-256 " + sha + " → " + (ok ? "정상" : "손상/다른 파일");
    }

    static String sha(byte[] b) throws Exception { StringBuilder h = new StringBuilder(); for (byte x : MessageDigest.getInstance("SHA-256").digest(b)) h.append(String.format("%02x", x)); return h.toString(); }
    static File put(File d, String n, byte[] b) throws Exception { File f = new File(d, n); Files.write(f.toPath(), b); return f; }
    static String snap(File d) throws Exception { StringBuilder s = new StringBuilder(); String[] l = d.list(); Arrays.sort(l); for (String n : l) { File f = new File(d, n); s.append(n).append(':').append(f.length()).append(':').append(f.lastModified()).append(':').append(f.isFile() && f.length() < (1 << 20) ? Arrays.hashCode(Files.readAllBytes(f.toPath())) : 0).append(';'); /* big sparse files: size+mtime only */ } return s.toString(); }

    public static void main(String[] a) throws Exception {
        check("the constants are the ones RomSlice defines (single source)", VmRomCheck.LENGTH == (int) RomSlice.EXPECTED_LENGTH && VmRomCheck.SHA256.equals(RomSlice.EXPECTED_SHA256));
        File d = Files.createTempDirectory("rom").toFile();
        byte[] good = new byte[1000]; new java.util.Random(1).nextBytes(good);
        byte[] bad = good.clone(); bad[5] ^= 1;
        String gs = sha(good);
        File fGood = put(d, "good", good), fBad = put(d, "bad", bad), fShort = put(d, "short", Arrays.copyOf(good, 999));
        File fMissing = new File(d, "missing"), fDir = new File(d, "dir"); fDir.mkdir();
        File[] cases = { fGood, fBad, fShort, fMissing, fDir };
        for (File f : cases) {
            check("isGenuine equals the old code for " + f.getName(), VmRomCheck.isGenuine(f, 1000, gs) == oldIsGenuine(f, 1000, gs));
            check("info equals the old code for " + f.getName(), VmRomCheck.info(f, 1000, gs).equals(oldInfo(f, 1000, gs)));
        }
        check("positive path: the exact file is genuine", VmRomCheck.isGenuine(fGood, 1000, gs));
        check("one flipped byte is not genuine", !VmRomCheck.isGenuine(fBad, 1000, gs));
        check("wrong size is not genuine", !VmRomCheck.isGenuine(fShort, 1000, gs));
        check("missing file and directory are not genuine", !VmRomCheck.isGenuine(fMissing, 1000, gs) && !VmRomCheck.isGenuine(fDir, 1000, gs));
        check("the real-constant overloads agree with the explicit ones for a non-ROM file", !VmRomCheck.isGenuine(fGood) && VmRomCheck.info(fGood).contains("손상/다른 파일"));

        // report: no genuine ROM anywhere (a synthetic file is never the real ROM), IPSW absent
        File ext = Files.createTempDirectory("ext").toFile(), priv = Files.createTempDirectory("priv").toFile(), lib = Files.createTempDirectory("lib").toFile();
        put(priv, "blank_disk.img", new byte[] {1});
        String be = snap(ext), bp = snap(priv);
        String r = VmInstalledDiag.report(ext, priv, lib);
        check("report: both ROM lines shown and 'no usable ROM' stated", r.contains("외부 vm/rom.bin: 없음") && r.contains("내부 AVPBooter.vmapple2.bin: 없음") && r.contains("사용할 ROM: 없음"));
        check("report says it downloads nothing and the IPSW is absent", r.contains("받지 않았") && r.contains("다운로드 파일 없음"));
        check("report never mentions the private blank image", !r.contains("blank_disk"));
        check("report changed no file (before == after)", be.equals(snap(ext)) && bp.equals(snap(priv)));
        // a wrong ROM file in both places is reported as corrupt, never as usable, and is not deleted
        put(ext, "rom.bin", good); put(priv, "AVPBooter.vmapple2.bin", good);
        be = snap(ext); bp = snap(priv);
        r = VmInstalledDiag.report(ext, priv, lib);
        check("a non-genuine ROM is reported 손상/다른 파일, not usable, and kept", r.contains("손상/다른 파일") && r.contains("사용할 ROM: 없음") && be.equals(snap(ext)) && bp.equals(snap(priv)));
        // IPSW: existence/size only; final file, .part and .meta in every combination
        File ip = new File(priv, VmInstalledDiag.IPSW_NAME), pt = new File(priv, VmInstalledDiag.IPSW_NAME + ".part"), mt = new File(priv, VmInstalledDiag.IPSW_NAME + ".meta");
        r = VmInstalledDiag.report(ext, priv, lib);
        check("no IPSW file of any kind -> 'no download files'", r.contains("다운로드 파일 없음") && !r.contains(".part:"));
        try (java.io.RandomAccessFile raf = new java.io.RandomAccessFile(pt, "rw")) { raf.setLength(777); }
        r = VmInstalledDiag.report(ext, priv, lib);
        check(".part alone: shown with its size, resume NOT claimed", r.contains("최종 파일: 없음") && r.contains(".part: 있음, 777 바이트") && r.contains(".meta: 없음") && r.contains("이어받기 가능 여부는 확인하지 않음") && !r.contains("이어받기 가능합니다"));
        put(priv, VmInstalledDiag.IPSW_NAME + ".meta", new byte[] {1, 2, 3});
        r = VmInstalledDiag.report(ext, priv, lib);
        check(".part + .meta: both shown, still no resume claim", r.contains(".part: 있음, 777 바이트") && r.contains(".meta: 있음, 3 바이트") && r.contains("이어받기 가능 여부는 확인하지 않음"));
        pt.delete(); mt.delete();
        try (java.io.RandomAccessFile raf = new java.io.RandomAccessFile(ip, "rw")) { raf.setLength(12345); }
        r = VmInstalledDiag.report(ext, priv, lib);
        check("final file of the wrong size: flagged as mismatching, NOT described as a partial download", r.contains("12345 바이트") && r.contains("불일치") && r.contains("확인 필요") && !r.contains("받다 만"));
        try (java.io.RandomAccessFile raf = new java.io.RandomAccessFile(ip, "rw")) { raf.setLength(VmInstalledDiag.IPSW_EXPECTED_BYTES); }
        r = VmInstalledDiag.report(ext, priv, lib);
        check("final file of the expected size: says SHA-256 check happens in the download procedure and is NOT re-verified here", r.contains("기대 크기와 일치") && r.contains("SHA-256 검증 후에만") && r.contains("현재 무결성을 검증하지 않음") && !r.contains("검증됨") && !r.contains(".part:"));
        put(priv, VmInstalledDiag.IPSW_NAME + ".part", new byte[] {9});
        r = VmInstalledDiag.report(ext, priv, lib);
        check("final file together with a .part: both reported", r.contains("기대 크기와 일치") && r.contains(".part: 있음, 1 바이트"));
        String bs = snap(priv); VmInstalledDiag.report(ext, priv, lib);
        check("the IPSW report changes no file (the multi-GB file is only sized, never opened)", bs.equals(snap(priv)));
        pt.delete(); ip.delete();
        // priority (synthetic expectations so the positive path is reachable): external genuine wins; internal is the fallback
        File e2 = Files.createTempDirectory("e2").toFile(), p2 = Files.createTempDirectory("p2").toFile();
        check("both bad/missing -> no usable ROM", VmInstalledDiag.usableRom(e2, p2, 1000, gs).startsWith("없음"));
        put(p2, "AVPBooter.vmapple2.bin", good);
        check("only the internal copy genuine -> internal is used", VmInstalledDiag.usableRom(e2, p2, 1000, gs).startsWith("앱 내부"));
        put(e2, "rom.bin", bad);
        check("external corrupt, internal genuine -> internal is used", VmInstalledDiag.usableRom(e2, p2, 1000, gs).startsWith("앱 내부"));
        put(e2, "rom.bin", good);
        check("both genuine -> external is preferred", VmInstalledDiag.usableRom(e2, p2, 1000, gs).startsWith("외부"));
        put(p2, "AVPBooter.vmapple2.bin", bad);
        check("external genuine, internal corrupt -> external", VmInstalledDiag.usableRom(e2, p2, 1000, gs).startsWith("외부"));
        System.out.println(fails == 0 ? "ALL PASS" : fails + " FAIL");
        System.exit(fails == 0 ? 0 : 1);
    }
}
