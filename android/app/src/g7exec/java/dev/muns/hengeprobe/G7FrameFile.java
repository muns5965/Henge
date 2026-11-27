package dev.muns.hengeprobe;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;

/**
 * integ7b T9: reader of the full-resolution picture file the runner writes on a "frame" request (host/g7_frame_file.h: "G7FRAME1" header, BGRA pixels, "G7FREND1" trailer, both with the picture's
 * sequence number; published whole by rename). Pure Java, no Android classes (a plain JVM test reads real files made by the C writer).
 *
 * Memory: one reusable band of rows (BAND_ROWS) and one reusable read buffer, whatever the picture size; the caller's sink (the activity's reused Bitmap, a test's array) is the only full copy.
 * Nothing is allocated per frame after the first. The picture is checked before any pixel is delivered (header magic, sizes against the file length, trailer magic and sequence number equal to the
 * header's) and again after the last band (the file must still say the same, and its length must not have changed): a file that was cut, mixed or rewritten in place is reported, not drawn.
 */
public final class G7FrameFile {
    private G7FrameFile() { }
    public static final String NAME = "g7_frame.bin";
    public static final int HDR = 32, TRL = 16, MAX_DIM = 4096;
    public static final int BAND_ROWS = 64;

    public static final int OK = 0, NO_FILE = 1, BAD_HEADER = 2, TORN = 3, TOO_BIG = 4, IO_ERROR = 5;
    public static String statusName(int s) { switch (s) { case OK: return "ok"; case NO_FILE: return "no file"; case BAD_HEADER: return "bad header or size"; case TORN: return "torn (header and trailer differ, or the file changed while reading)"; case TOO_BIG: return "too big"; default: return "io error"; } }

    public interface BandSink {
        /** rows x width ARGB pixels (alpha forced to 255) of rows y0..y0+rows-1, row-major, valid only during the call. */
        void band(int[] argb, int y0, int rows, int width);
    }

    /** A reader keeps its buffers between frames. Not thread safe: one thread at a time. */
    public static final class Reader {
        private byte[] raw = new byte[0];
        private int[] band = new int[0];
        public int width, height; public long seq;
        public int allocations;   // buffer (re)allocations so far, for the no-per-frame-allocation test

        /** Reads the picture in dir/NAME. Returns OK and fills width/height/seq, or a status; the sink got no pixel for any status but OK or TORN-after-the-last-check. */
        public int read(File f, BandSink sink) {
            try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
                long len = r.length();
                if (len < HDR + TRL) return BAD_HEADER;
                byte[] h = new byte[HDR]; // small, per frame; the pixel buffers are the reused ones
                r.readFully(h);
                if (!magic(h, 0, "G7FRAME1")) return BAD_HEADER;
                long w = u32(h, 8), ht = u32(h, 12), fmt = u32(h, 16), stride = u32(h, 20), seq0 = u64(h, 24);
                if (fmt != 1 || w < 1 || ht < 1 || stride != w * 4) return BAD_HEADER;
                if (w > MAX_DIM || ht > MAX_DIM) return TOO_BIG;
                if (len != HDR + stride * ht + TRL) return BAD_HEADER;
                if (!trailerOk(r, len, seq0)) return TORN;
                int iw = (int) w, ih = (int) ht;
                int need = iw * 4 * Math.min(BAND_ROWS, ih);
                if (raw.length < need) { raw = new byte[need]; allocations++; }
                if (band.length < iw * Math.min(BAND_ROWS, ih)) { band = new int[iw * Math.min(BAND_ROWS, ih)]; allocations++; }
                r.seek(HDR);
                for (int y = 0; y < ih; y += BAND_ROWS) {
                    int rows = Math.min(BAND_ROWS, ih - y), n = rows * iw * 4;
                    r.readFully(raw, 0, n);
                    for (int i = 0, o = 0; i < rows * iw; i++, o += 4)
                        band[i] = 0xff000000 | ((raw[o + 2] & 0xff) << 16) | ((raw[o + 1] & 0xff) << 8) | (raw[o] & 0xff);   // B G R A -> opaque ARGB
                    if (sink != null) sink.band(band, y, rows, iw);
                }
                // after the last band: still the same file? (the writer never changes a published file; a copy cut off or rewritten in place is not drawn as good)
                if (r.length() != len || !trailerOk(r, len, seq0)) return TORN;
                byte[] h2 = new byte[HDR]; r.seek(0); r.readFully(h2);
                if (u64(h2, 24) != seq0 || u32(h2, 8) != w || u32(h2, 12) != ht) return TORN;
                width = iw; height = ih; seq = seq0;
                return OK;
            } catch (java.io.FileNotFoundException e) { return NO_FILE;
            } catch (java.io.EOFException e) { return TORN;
            } catch (IOException | RuntimeException e) { return IO_ERROR; }
        }

        private static boolean trailerOk(RandomAccessFile r, long len, long seq) throws IOException {
            byte[] t = new byte[TRL];
            r.seek(len - TRL); r.readFully(t);
            return magic(t, 0, "G7FREND1") && u64(t, 8) == seq;
        }
    }

    static boolean magic(byte[] b, int o, String m) { for (int i = 0; i < m.length(); i++) if (b[o + i] != (byte) m.charAt(i)) return false; return true; }
    static long u32(byte[] b, int o) { return (b[o] & 0xffL) | ((b[o + 1] & 0xffL) << 8) | ((b[o + 2] & 0xffL) << 16) | ((b[o + 3] & 0xffL) << 24); }
    static long u64(byte[] b, int o) { return u32(b, o) | (u32(b, o + 4) << 32); }

    /** The runner's FRAMEFILE answer: ui_runner_vm_clone FRAMEFILE name=g7_frame.bin w=W h=H seq=N bytes=B format=bgra8888 stride=S. Null when it does not match exactly (a name other than NAME is refused: never a path). */
    public static final class Info {
        public final int w, h; public final long seq;
        Info(int w, int h, long seq) { this.w = w; this.h = h; this.seq = seq; }
    }
    private static final java.util.regex.Pattern LINE = java.util.regex.Pattern.compile("^ui_runner_vm_clone FRAMEFILE name=" + java.util.regex.Pattern.quote(NAME) + " w=(\\d{1,5}) h=(\\d{1,5}) seq=(\\d{1,19}) bytes=(\\d{1,12}) format=bgra8888 stride=(\\d{1,6})$");
    public static Info parseLine(String line) {
        if (line == null) return null;
        java.util.regex.Matcher m = LINE.matcher(line);
        if (!m.matches()) return null;
        long w = Long.parseLong(m.group(1)), h = Long.parseLong(m.group(2)), bytes = Long.parseLong(m.group(4)), stride = Long.parseLong(m.group(5));
        if (w < 1 || h < 1 || w > MAX_DIM || h > MAX_DIM || stride != w * 4 || bytes != HDR + stride * h + TRL) return null;
        long seq; try { seq = Long.parseLong(m.group(3)); } catch (NumberFormatException e) { return null; }
        return new Info((int) w, (int) h, seq);
    }
}
