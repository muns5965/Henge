package dev.muns.hengeprobe;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;

public class G7AuxCompareTest {
    static int failed = 0;
    static void check(String n, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + n); if (!ok) failed++; }

    public static void main(String[] a) throws Exception {
        Path dir = Files.createTempDirectory("auxt");
        byte[] base = new byte[33570816];
        base[0] = (byte) 0xa5;
        byte[] after = base.clone();
        byte[] w = "auto-boot\0true\0boot-command\0recoveryd".getBytes("US-ASCII");
        System.arraycopy(w, 0, after, G7AuxCompare.DATA_OFFSET + 512 * 64 + 8, w.length);
        System.arraycopy(w, 0, after, G7AuxCompare.DATA_OFFSET + 512 * 65, 5);
        Files.write(dir.resolve("macos_aux.initial"), base);
        Files.write(dir.resolve("macos_aux.img"), after);
        String r = G7AuxCompare.compareInitialToImage(dir.toFile());
        check("changed sectors counted (64..65 = 2 sectors, one range)", r.contains("바뀐 섹터(512바이트): 2개, 구간 1개") && r.contains("구간 64~65"));
        check("header reported equal", r.contains("머리 16 KiB: 같음"));
        check("boot words found in the changed range", r.contains("auto-boot") && r.contains("boot-command"));
        check("report says it is only a string search", r.contains("글자열 검색일 뿐"));
        Files.write(dir.resolve("macos_aux.img"), base);
        check("identical files: no change reported", G7AuxCompare.compareInitialToImage(dir.toFile()).contains("바뀐 곳이 없습니다"));
        Files.write(dir.resolve("macos_aux.img"), after);
        Path cl = Files.createDirectory(dir.resolve("clone-001"));
        Files.write(cl.resolve("macos_aux.img"), after);
        check("clone equal to the original's AUX", G7AuxCompare.compareCloneToOriginal(dir.toFile(), cl.toFile()).contains("같음"));
        Files.write(cl.resolve("macos_aux.img"), base);
        check("clone that differs is reported", G7AuxCompare.compareCloneToOriginal(dir.toFile(), cl.toFile()).contains("원본과 다름"));
        Files.delete(dir.resolve("macos_aux.initial"));
        check("missing file: reported, no exception", G7AuxCompare.compareInitialToImage(dir.toFile()).contains("비교하지 못했습니다"));
        Path ln = dir.resolve("macos_aux.initial");
        Files.createSymbolicLink(ln, dir.resolve("macos_aux.img"));
        check("a symlink is refused", G7AuxCompare.compareInitialToImage(dir.toFile()).contains("일반 파일이 아닙니다"));
        long before = new File(dir.toFile(), "macos_aux.img").lastModified();
        check("inputs untouched", new File(dir.toFile(), "macos_aux.img").lastModified() == before);
        System.out.println(failed == 0 ? "G7AUXCOMPARE OK" : "G7AUXCOMPARE FAILED " + failed);
        System.exit(failed == 0 ? 0 : 1);
    }
}
