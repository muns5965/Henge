package dev.muns.hengeprobe;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.TimeUnit;

/** G7CloneBoot on a plain JVM with a fake process: nothing real is started. */
public class G7CloneBootTest {
    static int fails, spawned;
    static void check(String n, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + n); if (!ok) fails++; }

    static final class Fake extends Process {
        final byte[] out; final int exit; final boolean hang; volatile boolean destroyed; final ByteArrayOutputStream in = new ByteArrayOutputStream();
        volatile boolean quitSeen;
        Fake(String o, int e, boolean h) { out = o.getBytes(StandardCharsets.UTF_8); exit = e; hang = h; }
        @Override public OutputStream getOutputStream() { return new OutputStream() { @Override public void write(int b) { in.write(b); if (new String(in.toByteArray()).contains("quit")) quitSeen = true; } }; }
        @Override public InputStream getInputStream() {
            if (!hang) return new ByteArrayInputStream(out);
            return new InputStream() { @Override public int read() { while (!destroyed && !quitSeen) { try { Thread.sleep(5); } catch (InterruptedException e) { return -1; } } return -1; } @Override public int read(byte[] b, int o, int l) { return read(); } };
        }
        @Override public InputStream getErrorStream() { return new ByteArrayInputStream(new byte[0]); }
        @Override public int waitFor() { while (hang && !destroyed && !quitSeen) { try { Thread.sleep(5); } catch (InterruptedException e) { break; } } return exit; }
        @Override public boolean waitFor(long t, TimeUnit u) { if (!hang) return true; for (int i = 0; i < 400 && !destroyed && !quitSeen; i++) { try { Thread.sleep(5); } catch (InterruptedException e) { break; } } return destroyed || quitSeen; }
        @Override public int exitValue() { return exit; }
        @Override public void destroy() { destroyed = true; }
    }
    static List<String> lastCmd;
    static G7CloneBoot.Spawner sp(Fake p) { return c -> { spawned++; lastCmd = c; return p; }; }
    static File rom;
    static final G7ExecLauncher.RomFinder ROMS = (e, p) -> rom;
    static void waitDone(G7CloneBoot.Run r) throws Exception { for (int i = 0; i < 600 && !r.finished(); i++) Thread.sleep(10); }

    public static void main(String[] x) throws Exception {
        File priv = G7CloneTest.pair("in_progress");
        File lib = Files.createTempDirectory("lib").toFile(), ext = Files.createTempDirectory("ext").toFile(), files = Files.createTempDirectory("files").toFile();
        rom = new File(priv, "AVPBooter.vmapple2.bin"); Files.write(rom.toPath(), new byte[] {1, 2, 3});
        Files.write(new File(priv, "vm_ecid.txt").toPath(), ("ecid=0x" + Long.toHexString(G7CloneTest.ECID) + "\n").getBytes());
        Files.write(new File(lib, G7CloneBoot.LIB_NAME).toPath(), new byte[] {1});
        String before = G7CloneTest.snapshot(priv);
        G7Clone.Result cr = G7CloneTest.run(true, false, priv, 0, G7CloneTest.SCAN, 1L << 40, () -> false);
        File clone = cr.dir; check("setup: a verified clone exists", cr.ok && clone != null);

        java.util.function.IntSupplier none = () -> 0;
        Fake f0 = new Fake("", 0, false);
        check("switch off: refused, nothing spawned", !G7CloneBoot.startWith(sp(f0), ROMS, false, () -> false, lib, ext, priv, files, clone, true, none).started && spawned == 0);
        check("no consent: refused", !G7CloneBoot.startWith(sp(f0), ROMS, true, () -> false, lib, ext, priv, files, clone, false, none).started && spawned == 0);
        check("a G7 run is live: refused", !G7CloneBoot.startWith(sp(f0), ROMS, true, () -> true, lib, ext, priv, files, clone, true, none).started && spawned == 0);
        check("a runner process is present: refused", !G7CloneBoot.startWith(sp(f0), ROMS, true, () -> false, lib, ext, priv, files, clone, true, () -> 1).started && spawned == 0);
        check("runner state unknown: refused", !G7CloneBoot.startWith(sp(f0), ROMS, true, () -> false, lib, ext, priv, files, clone, true, () -> -1).started && spawned == 0);
        check("the ORIGINAL folder is refused (not a clone)", !G7CloneBoot.startWith(sp(f0), ROMS, true, () -> false, lib, ext, priv, files, priv, true, none).started && spawned == 0);
        File noLib = Files.createTempDirectory("nolib").toFile();
        check("binary not packaged: refused", !G7CloneBoot.startWith(sp(f0), ROMS, true, () -> false, noLib, ext, priv, files, clone, true, none).started && spawned == 0);
        File mk = new File(clone, "clone_state.txt"); byte[] mkb = Files.readAllBytes(mk.toPath());
        Files.write(mk.toPath(), "state=clone_incomplete\n".getBytes());
        check("incomplete marker: refused", !G7CloneBoot.startWith(sp(f0), ROMS, true, () -> false, lib, ext, priv, files, clone, true, none).started && spawned == 0);
        Files.write(mk.toPath(), mkb);
        Files.write(new File(priv, "vm_ecid.txt").toPath(), ("ecid=0x" + Long.toHexString(G7CloneTest.ECID + 1) + "\n").getBytes());
        check("ECID mismatch: refused", !G7CloneBoot.startWith(sp(f0), ROMS, true, () -> false, lib, ext, priv, files, clone, true, none).started && spawned == 0);
        Files.write(new File(priv, "vm_ecid.txt").toPath(), ("ecid=0x" + Long.toHexString(G7CloneTest.ECID) + "\n").getBytes());
        rom = null;
        check("no ROM: refused", !G7CloneBoot.startWith(sp(f0), ROMS, true, () -> false, lib, ext, priv, files, clone, true, none).started && spawned == 0);
        rom = new File(priv, "AVPBooter.vmapple2.bin");

        String hex = Long.toHexString(G7CloneTest.ECID);
        Fake ok = new Fake("hengevm: cpu 0 pc sample #1 pc=0x100 el=3\nui_runner_vm_clone frames published 4\nleak " + hex + "\n", 0, false);
        G7CloneBoot.Start s = G7CloneBoot.startWith(sp(ok), ROMS, true, () -> false, lib, ext, priv, files, clone, true, none);
        check("a valid clone starts: " + s.message, s.started && spawned == 1 && s.run != null);
        String cmd = String.join(" ", lastCmd);
        check("command: clone dir, ecid-dir, bounded; no ecid value, no original files", cmd.contains("--clone-dir " + clone.getAbsolutePath()) && cmd.contains("--ecid-dir " + priv.getPath()) && cmd.contains("--seconds " + G7CloneBoot.RUN_SECONDS) && !cmd.contains(hex) && !cmd.contains("--ecid ") && !cmd.contains("macos_disk.img"));
        waitDone(s.run);
        File log = G7ExecLauncher.logFile(files, s.run.logName());
        String lt = new String(Files.readAllBytes(log.toPath()), StandardCharsets.UTF_8);
        check("log exists with the g7_cboot_ name, has exit line and stage summary, ECID masked", s.run.logName().matches("g7_cboot_[0-9]{13}\\.txt") && lt.contains("g7-cboot-exit: 0") && !lt.contains(hex) && lt.contains("g7-stage:"));
        check("finished run frees the gate", s.run.finished() && !G7ExecLauncher.hasLiveRun());

        Fake hang = new Fake("", 0, true);
        s = G7CloneBoot.startWith(sp(hang), ROMS, true, () -> G7ExecLauncher.hasLiveRun(), lib, ext, priv, files, clone, true, none);
        check("second start while running", s.started);
        check("while a clone boot is live the gates see it: G7 start / clone / second boot refused", G7ExecLauncher.hasLiveRun()
            && !G7CloneBoot.startWith(sp(f0), ROMS, true, G7ExecLauncher::hasLiveRun, lib, ext, priv, files, clone, true, none).started
            && !G7Clone.cloneWith(true, G7ExecLauncher::hasLiveRun, priv, none, G7CloneTest.SCAN, G7CloneTest.SIZER, () -> 1L << 40, () -> false).ok);
        s.run.cancel(); waitDone(s.run);
        check("cancel: runner asked to quit, run ended, marked cancelled", hang.quitSeen && s.run.finished() && new String(Files.readAllBytes(new File(files, s.run.logName()).toPath())).contains("(cancelled)"));
        check("the original pair and the clone marker are unchanged by all of this", G7CloneTest.snapshot(priv).equals(before));
        System.out.println(fails == 0 ? "G7CLONEBOOT OK" : "G7CLONEBOOT FAILED " + fails);
        System.exit(fails == 0 ? 0 : 1);
    }
}
