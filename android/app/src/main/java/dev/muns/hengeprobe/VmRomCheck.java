package dev.muns.hengeprobe;

import java.io.File;
import java.io.FileInputStream;

/** The one place that decides whether a file is the genuine AVPBooter ROM (exact size and SHA-256, both defined once in
 *  RomSlice), shared by MainActivity (which picks/downloads the ROM) and the read-only [A-INST] report so the two can never
 *  disagree. Reading only: no network, nothing is created, changed or deleted. Moved here unchanged from MainActivity
 *  (isGenuineRom / romInfo); tests/java/VmRomCheckTest.java compares it with a verbatim copy of the old code.
 *  No Android classes: plain-JVM testable. */
final class VmRomCheck {
    private VmRomCheck() {}

    static final int LENGTH = (int) RomSlice.EXPECTED_LENGTH;
    static final String SHA256 = RomSlice.EXPECTED_SHA256;

    /** True if the file is exactly the known AVPBooter ROM (size and SHA-256). */
    static boolean isGenuine(File f) { return isGenuine(f, LENGTH, SHA256); }

    /** Same decision against explicit expectations (so a test can exercise the positive path with a synthetic file). */
    static boolean isGenuine(File f, int length, String sha256) {
        if (!f.isFile() || f.length() != length) return false;
        try (FileInputStream in = new FileInputStream(f)) {
            java.security.MessageDigest md = java.security.MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) md.update(buf, 0, n);
            StringBuilder hex = new StringBuilder();
            for (byte b : md.digest()) hex.append(String.format("%02x", b));
            return hex.toString().equals(sha256);
        } catch (Exception e) {
            return false;
        }
    }

    /** One line about a ROM candidate: existence, size, SHA-256 verdict. */
    static String info(File f) { return info(f, LENGTH, SHA256); }

    static String info(File f, int length, String sha256) {
        if (!f.exists()) return "없음";
        if (!f.isFile()) return "파일이 아님";
        String sha = "?";
        try (FileInputStream in = new FileInputStream(f)) {
            java.security.MessageDigest md = java.security.MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) md.update(buf, 0, n);
            StringBuilder hex = new StringBuilder();
            for (byte b : md.digest()) hex.append(String.format("%02x", b));
            sha = hex.toString();
        } catch (Exception e) {
            sha = "읽기 실패: " + e;
        }
        boolean ok = f.length() == length && sha.equals(sha256);
        return "있음, " + f.length() + "바이트 (기대 " + length + "), SHA-256 " + sha + " → " + (ok ? "정상" : "손상/다른 파일");
    }
}
