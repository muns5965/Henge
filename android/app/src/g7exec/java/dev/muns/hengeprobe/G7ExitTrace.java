package dev.muns.hengeprobe;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.StandardOpenOption;

/** Keeps the binary trace of an exit record (a native crash is a protobuf tombstone) as a new file, never over an existing one. Plain Java: tested on a JVM. */
final class G7ExitTrace {
    private G7ExitTrace() {}
    static final long MAX_BYTES = 1L << 20;

    /** Copies at most MAX_BYTES of `in` to dir/name (created with CREATE_NEW). Returns the size kept, or -1 when the file already existed (it is left alone). */
    static long keep(InputStream in, File dir, String name) throws IOException {
        if (!name.matches("tombstone_[0-9]{1,10}_[0-9]{1,16}\\.pb")) throw new IOException("bad name");
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("no directory");
        File f = new File(dir, name);
        if (f.exists()) return -1;
        long total = 0;
        try (OutputStream o = Files.newOutputStream(f.toPath(), StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE)) {
            byte[] buf = new byte[8192]; int n;
            while (total < MAX_BYTES && (n = in.read(buf, 0, (int) Math.min(buf.length, MAX_BYTES - total))) > 0) { o.write(buf, 0, n); total += n; }
        }
        return total;
    }
}
