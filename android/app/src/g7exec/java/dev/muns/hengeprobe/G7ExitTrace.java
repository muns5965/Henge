package dev.muns.hengeprobe;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.StandardOpenOption;

/** Keeps the binary trace of an exit record (a native crash is a protobuf tombstone) as a new file, never over an existing one, and never beyond a fixed number of files and total size. Plain Java: tested on a JVM. Nothing is ever deleted here. */
final class G7ExitTrace {
    private G7ExitTrace() {}
    static final long MAX_BYTES = 1L << 20;          // one file
    static final int MAX_FILES = 5;                  // kept files in the folder
    static final long MAX_TOTAL_BYTES = 4L << 20;    // all kept files together

    enum Status { KEPT, KEPT_TRUNCATED, ALREADY_THERE, LIMIT_REACHED }

    static final class Result {
        final Status status; final long bytes;
        Result(Status s, long b) { status = s; bytes = b; }
    }

    /** Copies at most MAX_BYTES of `in` to dir/name (CREATE_NEW). KEPT_TRUNCATED when the trace had more than MAX_BYTES (the file is a cut-off record and says so only here); ALREADY_THERE when the name exists (left alone, so a repeated look does not store it again); LIMIT_REACHED when the folder already holds MAX_FILES files or MAX_TOTAL_BYTES (nothing is written, nothing is removed). */
    static Result keep(InputStream in, File dir, String name) throws IOException {
        if (!name.matches("tombstone_[0-9]{1,10}_[0-9]{1,16}\\.pb")) throw new IOException("bad name");
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("no directory");
        File f = new File(dir, name);
        if (f.exists()) return new Result(Status.ALREADY_THERE, f.length());
        File[] have = dir.listFiles();
        long sum = 0; int count = 0;
        if (have != null) for (File h : have) if (h.isFile()) { count++; sum += h.length(); }
        if (count >= MAX_FILES || sum >= MAX_TOTAL_BYTES) return new Result(Status.LIMIT_REACHED, 0);
        long room = Math.min(MAX_BYTES, MAX_TOTAL_BYTES - sum), total = 0;
        boolean more = false;
        try (OutputStream o = Files.newOutputStream(f.toPath(), StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE)) {
            byte[] buf = new byte[8192]; int n;
            while (total < room && (n = in.read(buf, 0, (int) Math.min(buf.length, room - total))) > 0) { o.write(buf, 0, n); total += n; }
            more = total >= room && in.read() >= 0;
        }
        return new Result(more ? Status.KEPT_TRUNCATED : Status.KEPT, total);
    }
}
