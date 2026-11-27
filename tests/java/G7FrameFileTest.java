package dev.muns.hengeprobe;

import java.io.File;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;

/** integ7b T9: the app's reader of the runner's full-resolution picture file, the coordinate mapping both ways, torn-read protection and the low-resolution diagnostic that stays. Arg 0: the directory tests/test_g7_framefile.c wrote. */
public class G7FrameFileTest {
    static int fail;
    static void check(boolean ok, String what) { System.out.println((ok ? "PASS " : "FAIL ") + what); if (!ok) fail++; }

    static byte[] file(int w, int h, long seqHead, long seqTail, int value) {
        byte[] b = new byte[G7FrameFile.HDR + w * h * 4 + G7FrameFile.TRL];
        System.arraycopy("G7FRAME1".getBytes(StandardCharsets.US_ASCII), 0, b, 0, 8);
        put32(b, 8, w); put32(b, 12, h); put32(b, 16, 1); put32(b, 20, w * 4); put64(b, 24, seqHead);
        for (int i = 0; i < w * h; i++) { int o = G7FrameFile.HDR + i * 4; b[o] = (byte) value; b[o + 1] = (byte) (value >> 8); b[o + 2] = (byte) (value >> 16); b[o + 3] = (byte) 0x7f; }
        int t = G7FrameFile.HDR + w * h * 4;
        System.arraycopy("G7FREND1".getBytes(StandardCharsets.US_ASCII), 0, b, t, 8); put64(b, t + 8, seqTail);
        return b;
    }
    static void put32(byte[] b, int o, long v) { for (int i = 0; i < 4; i++) b[o + i] = (byte) (v >> (8 * i)); }
    static void put64(byte[] b, int o, long v) { for (int i = 0; i < 8; i++) b[o + i] = (byte) (v >> (8 * i)); }

    public static void main(String[] a) throws Exception {
        File dir = new File(a[0]);
        List<String> exp = Files.readAllLines(new File(dir, "expected.txt").toPath());
        String[] meta = exp.get(0).split(" ");
        int W = Integer.parseInt(meta[1]), H = Integer.parseInt(meta[2]); long seq = Long.parseLong(meta[3]);

        // 1. the real picture, written by the C runner side from a shared-file round trip: exact pixels at known coordinates
        G7FrameFile.Reader rd = new G7FrameFile.Reader();
        final int[] full = new int[W * H];
        int st = rd.read(new File(dir, G7FrameFile.NAME), (argb, y0, rows, bw) -> System.arraycopy(argb, 0, full, y0 * bw, rows * bw));
        check(st == G7FrameFile.OK && rd.width == 1920 && rd.height == 1080 && rd.seq == seq, "1920x1080 at its own size (not resampled), sequence number " + rd.seq + " as the shared file had it");
        int bad = 0, n = 0;
        for (String l : exp) {
            if (!l.startsWith("px ")) continue;
            String[] p = l.split(" "); int x = Integer.parseInt(p[1]), y = Integer.parseInt(p[2]);
            int B = Integer.parseInt(p[3]), G = Integer.parseInt(p[4]), R = Integer.parseInt(p[5]);
            int want = 0xff000000 | (R << 16) | (G << 8) | B;   // alpha is forced opaque
            n++; if (full[y * W + x] != want) { bad++; if (bad < 5) System.out.println("  pixel " + x + "," + y + " got " + Integer.toHexString(full[y * W + x]) + " want " + Integer.toHexString(want)); }
        }
        check(n >= 400 && bad == 0, "exact pixel values at " + n + " known coordinates (corner markers, colour bars, ring, strokes, random points): " + bad + " wrong");
        check(full[0] == 0xffff0000 && full[W - 1] == 0xff00ff00 && full[(H - 1) * W] == 0xff0000ff && full[H * W - 1] == 0xffff8000, "the four corner markers are in the right corners (red, green, blue, orange)");
        check(full[130 * W + 120] == 0xffffffff && full[130 * W + 360] == 0xffffff00, "colour bars: white then yellow, left to right");
        // a single-pixel stroke survives (the old 96x54 copy could not hold it)
        int strokes = 0; for (int x = 300; x < 620; x++) if (full[850 * W + x] == 0xffe6e6e6) strokes++;
        check(strokes > 30, "single-pixel strokes of the text-like row are present (" + strokes + " light pixels in one row)");

        // 2. no per-frame allocation: the buffers are made on the first read only
        int alloc = rd.allocations;
        for (int i = 0; i < 5; i++) rd.read(new File(dir, G7FrameFile.NAME), null);
        check(rd.allocations == alloc && alloc == 2, "five more reads allocate nothing new (the reader's two buffers: " + alloc + ")");

        // 3. coordinate mapping, both ways, letterboxed in portrait and landscape views (rotation = the view's size at touch time) and with another picture size
        int[][] views = {{1080, 2340}, {2340, 1080}, {1080, 1080}, {500, 281}, {1920, 1080}, {720, 240 * 3}};
        int mapBad = 0, nm = 0;
        for (int[] v : views) {
            double[] r = G7InputMap.fitRect(v[0], v[1], 1920, 1080);
            if (Math.abs(r[2] / r[3] - 1920.0 / 1080.0) > 1e-9) mapBad++;
            if (r[0] < -1e-9 || r[1] < -1e-9 || r[0] + r[2] > v[0] + 1e-6 || r[1] + r[3] > v[1] + 1e-6) mapBad++;
            for (int gx = 0; gx < 1920; gx += 37) for (int gy = 0; gy < 1080; gy += 29) {
                float[] pt = G7InputMap.guestToPreview(gx, gy, v[0], v[1], 1920, 1080, 1920, 1080);
                int[] back = pt == null ? null : G7InputMap.previewToGuest(pt[0], pt[1], v[0], v[1], 1920, 1080, 1920, 1080);
                nm++;
                // the view is smaller than the picture in some cases: several guest pixels share a view pixel, so the round trip must land on the same pixel only when the scale is >= 1; always within the scale
                double scale = Math.min((double) v[0] / 1920, (double) v[1] / 1080);
                if (back == null || Math.abs(back[0] - gx) > Math.ceil(1.0 / scale) || Math.abs(back[1] - gy) > Math.ceil(1.0 / scale)) mapBad++;
            }
        }
        check(mapBad == 0, "view <-> guest mapping round trips within one view pixel in " + views.length + " view sizes (portrait, landscape, square, tiny): " + nm + " points, " + mapBad + " bad");
        int[] c0 = G7InputMap.previewToGuest(540, 1170, 1080, 2340, 1920, 1080, 1920, 1080);
        check(c0 != null && c0[0] == 960 && c0[1] == 540, "centre of a portrait view = guest (960,540)");
        check(G7InputMap.previewToGuest(540, 10, 1080, 2340, 1920, 1080, 1920, 1080) == null && G7InputMap.previewToGuest(540, 2330, 1080, 2340, 1920, 1080, 1920, 1080) == null, "a touch in the letterbox margin above/below the picture is not a guest click");
        check(G7InputMap.previewToGuest(10, 540, 2340, 1080, 1920, 1080, 1920, 1080) == null, "margin left of the picture in a landscape view too");
        double[] pr = G7InputMap.fitRect(1080, 2340, 1920, 1080);
        int[] tl = G7InputMap.previewToGuest((float) pr[0] + 0.01f, (float) pr[1] + 0.01f, 1080, 2340, 1920, 1080, 1920, 1080), br = G7InputMap.previewToGuest((float) (pr[0] + pr[2]) - 0.01f, (float) (pr[1] + pr[3]) - 0.01f, 1080, 2340, 1920, 1080, 1920, 1080);
        check(tl != null && tl[0] == 0 && tl[1] == 0 && br != null && br[0] == 1919 && br[1] == 1079, "the picture's own corners map to guest (0,0) and (1919,1079)");
        int[] o = G7InputMap.previewToGuest(1000, 500, 2000, 1000, 1024, 768, 1024, 768);   // a different frame size (early framebuffer): its own pixels are the coordinate space
        double[] or = G7InputMap.fitRect(2000, 1000, 1024, 768);
        check(o != null && o[0] >= 0 && o[0] < 1024 && o[1] >= 0 && o[1] < 768 && Math.abs(or[2] / or[3] - 1024.0 / 768) < 1e-9, "a 1024x768 picture keeps its 4:3 aspect and maps into 0..1023 x 0..767");

        // 4. torn / bad files are reported and no pixel is delivered
        File tf = new File(dir, "t.bin");
        int[] delivered = {0};
        G7FrameFile.BandSink count = (ar, y0, rows, bw) -> delivered[0]++;
        Files.write(tf.toPath(), file(64, 48, 5, 6, 0x112233));
        delivered[0] = 0; check(rd.read(tf, count) == G7FrameFile.TORN && delivered[0] == 0, "header seq 5, trailer seq 6: torn, nothing drawn");
        byte[] ok = file(64, 48, 7, 7, 0x112233); Files.write(tf.toPath(), ok);
        delivered[0] = 0; check(rd.read(tf, count) == G7FrameFile.OK && delivered[0] == 1 && rd.seq == 7, "a consistent small file reads");
        Files.write(tf.toPath(), java.util.Arrays.copyOf(ok, ok.length - 40));
        check(rd.read(tf, count) == G7FrameFile.BAD_HEADER, "a cut-off file (length does not match the header)");
        byte[] m = ok.clone(); m[0] = 'X'; Files.write(tf.toPath(), m); check(rd.read(tf, count) == G7FrameFile.BAD_HEADER, "wrong magic");
        m = ok.clone(); put32(m, 8, 0); Files.write(tf.toPath(), m); check(rd.read(tf, count) == G7FrameFile.BAD_HEADER, "width 0");
        m = ok.clone(); put32(m, 20, 64 * 4 + 4); Files.write(tf.toPath(), m); check(rd.read(tf, count) == G7FrameFile.BAD_HEADER, "stride that is not width*4");
        m = ok.clone(); put32(m, 16, 2); Files.write(tf.toPath(), m); check(rd.read(tf, count) == G7FrameFile.BAD_HEADER, "unknown pixel format");
        m = ok.clone(); put32(m, 8, 5000); put32(m, 20, 20000); Files.write(tf.toPath(), m); check(rd.read(tf, count) == G7FrameFile.TOO_BIG, "wider than 4096 is refused");
        m = ok.clone(); m[ok.length - 16] = 'Z'; Files.write(tf.toPath(), m); check(rd.read(tf, count) == G7FrameFile.TORN, "trailer magic wrong");
        check(rd.read(new File(dir, "none.bin"), count) == G7FrameFile.NO_FILE, "a missing file is 'no file'");

        // 5. the writer updates while the reader reads. (a) as the runner does (temp file + rename): every OK result is one whole picture
        final File live = new File(dir, "live.bin"), liveTmp = new File(dir, "live.tmp");
        Files.write(live.toPath(), file(320, 180, 1, 1, 0x010101));
        final java.util.concurrent.atomic.AtomicBoolean stop = new java.util.concurrent.atomic.AtomicBoolean(false);
        Thread w1 = new Thread(() -> { try { for (int s = 2; s < 400 && !stop.get(); s++) { Files.write(liveTmp.toPath(), file(320, 180, s, s, s * 0x010101)); Files.move(liveTmp.toPath(), live.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING, java.nio.file.StandardCopyOption.ATOMIC_MOVE); } } catch (Exception e) { System.out.println("writer: " + e); } });
        w1.start();
        int oks = 0, mixed = 0, torn = 0, other = 0;
        while (w1.isAlive()) {
            final int[] vals = new int[2]; final boolean[] first = {true}; final boolean[] mix = {false};
            int s2 = rd.read(live, (ar, y0, rows, bw) -> { for (int i = 0; i < rows * bw; i++) { if (first[0]) { vals[0] = ar[i]; first[0] = false; } else if (ar[i] != vals[0]) mix[0] = true; } });
            if (s2 == G7FrameFile.OK) { oks++; if (mix[0] || (vals[0] & 0xffffff) != ((int) rd.seq * 0x010101 & 0xffffff)) { mixed++; if (mixed < 4) System.out.println("  mixed: mix=" + mix[0] + " first=" + Integer.toHexString(vals[0]) + " seq=" + rd.seq); } } else if (s2 == G7FrameFile.TORN) torn++; else other++;
        }
        stop.set(true); w1.join();
        check(oks > 20 && mixed == 0 && other == 0, "rename writer racing the reader: " + oks + " whole pictures, " + mixed + " mixed, " + torn + " torn reports, " + other + " other errors");
        // (b) a writer that rewrites the SAME file in place (header first, trailer last): the reader may report torn, but never returns a mix as OK
        final File inplace = new File(dir, "inplace.bin");
        Files.write(inplace.toPath(), file(320, 180, 1, 1, 0x010101));
        final java.util.concurrent.atomic.AtomicBoolean stop2 = new java.util.concurrent.atomic.AtomicBoolean(false);
        Thread w2 = new Thread(() -> { try (RandomAccessFile rw = new RandomAccessFile(inplace, "rw")) { for (int s = 2; s < 300 && !stop2.get(); s++) { byte[] f = file(320, 180, s, s, s * 0x010101); rw.seek(0); rw.write(f, 0, G7FrameFile.HDR); for (int off = G7FrameFile.HDR; off < f.length - G7FrameFile.TRL; off += 4096) { rw.seek(off); rw.write(f, off, Math.min(4096, f.length - G7FrameFile.TRL - off)); } rw.seek(f.length - G7FrameFile.TRL); rw.write(f, f.length - G7FrameFile.TRL, G7FrameFile.TRL); } } catch (Exception e) { System.out.println("writer2: " + e); } });
        w2.start();
        int oks2 = 0, mixed2 = 0, torn2 = 0;
        while (w2.isAlive()) {
            final int[] vals = new int[1]; final boolean[] first = {true}; final boolean[] mix = {false};
            int s2 = rd.read(inplace, (ar, y0, rows, bw) -> { for (int i = 0; i < rows * bw; i++) { if (first[0]) { vals[0] = ar[i]; first[0] = false; } else if (ar[i] != vals[0]) mix[0] = true; } });
            if (s2 == G7FrameFile.OK) { oks2++; if (mix[0]) mixed2++; } else torn2++;
        }
        stop2.set(true); w2.join();
        check(mixed2 == 0, "in-place writer racing the reader: no mixed picture returned as OK (" + oks2 + " ok, " + torn2 + " reported torn)");

        // 6. the runner's FRAMEFILE answer line: exact format only, the name is a constant (never a path)
        String good = "ui_runner_vm_clone FRAMEFILE name=g7_frame.bin w=1920 h=1080 seq=42 bytes=" + (32 + 1920 * 4 * 1080 + 16) + " format=bgra8888 stride=7680";
        G7FrameFile.Info gi = G7FrameFile.parseLine(good);
        check(gi != null && gi.w == 1920 && gi.h == 1080 && gi.seq == 42, "FRAMEFILE line parses");
        check(G7FrameFile.parseLine(good.replace("g7_frame.bin", "../x.bin")) == null && G7FrameFile.parseLine(good.replace("w=1920", "w=9000")) == null && G7FrameFile.parseLine(good.replace("seq=42", "seq=42 extra")) == null
              && G7FrameFile.parseLine(good.replace("bytes=8294448", "bytes=8294449")) == null && G7FrameFile.parseLine("ui_runner_vm_clone FRAMEFILE failed errno=28 (No space left on device)") == null && G7FrameFile.parseLine(null) == null, "a path-like name, an oversized width, extra text, wrong byte count, the failure line and null are all refused");

        System.out.println(fail == 0 ? "G7FRAMEFILE OK" : "G7FRAMEFILE FAILED (" + fail + ")");
        System.exit(fail == 0 ? 0 : 1);
    }
}
