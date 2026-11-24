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
        File broken = G7CloneTest.pair("in_progress");
        G7Clone.Result br = G7CloneTest.run(true, false, broken, 0, G7CloneTest.SCAN, 1L << 40, () -> false);
        File bd = br.dir; File bdisk = new File(bd, "macos_disk.img"); byte[] keepDisk = Files.readAllBytes(bdisk.toPath()); Files.delete(bdisk.toPath());
        Files.write(new File(broken, "vm_ecid.txt").toPath(), ("ecid=0x" + Long.toHexString(G7CloneTest.ECID) + "\n").getBytes());
        Files.write(new File(broken, "AVPBooter.vmapple2.bin").toPath(), new byte[] {1});
        check("a clone whose disk is missing is refused by validate (explicitly) and shown as not bootable", G7Clone.validate(bd, broken, G7CloneTest.ECID) != null && G7CloneBoot.describeAll(broken).toString().contains("부팅 불가"));
        File viaLink = new File(Files.createTempDirectory("lnk").toFile(), "files"); Files.createSymbolicLink(viaLink.toPath(), priv.toPath());
        check("the app folder reached through a symbolic link (as /data/user/0 on Android) still lists its clone as bootable", G7CloneBoot.listValid(viaLink).size() == 1 && G7CloneBoot.describeAll(viaLink).toString().contains("부팅 가능"));
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
        StringBuilder px = new StringBuilder(); for (int i = 0; i < 4 * 2; i++) px.append(i == 0 ? "ffffff" : "000000");
        String frameLine = "ui_runner_vm_clone FRAME src=1920x1080 out=4x2 class=mostly-black-small-content nonblack_pct=12 colors=2 estimate=pixel-statistics-only data=" + px;
        Fake ok = new Fake("hengevm: cpu 0 pc sample #1 pc=0x100 el=3\nui_runner_vm_clone frames published 4\nleak " + hex + "\n" + frameLine + "\nui_runner_vm_clone RESULT: stopped, confirmed (time limit), ignored input lines 0\nui_runner_vm_clone VERDICT: vm_stop_confirmed=1 stop_kind=time-limit disk_flush=ok macos_boot=not-determined (x)\n", 0, false);
        G7CloneBoot.Start s = G7CloneBoot.startWith(sp(ok), ROMS, true, () -> false, lib, ext, priv, files, clone, true, none);
        check("a valid clone starts: " + s.message, s.started && spawned == 1 && s.run != null);
        String cmd = String.join(" ", lastCmd);
        check("command: clone dir, ecid-dir, bounded; no ecid value, no original files", cmd.contains("--clone-dir " + clone.getAbsolutePath()) && cmd.contains("--ecid-dir " + priv.getPath()) && cmd.contains("--seconds " + G7CloneBoot.RUN_SECONDS) && !cmd.contains(hex) && !cmd.contains("--ecid ") && !cmd.contains("macos_disk.img"));
        check("default usb mode is A (0) on the command line", cmd.contains("--usb-mode 0"));
        waitDone(s.run);
        File log = G7ExecLauncher.logFile(files, s.run.logName());
        String lt = new String(Files.readAllBytes(log.toPath()), StandardCharsets.UTF_8);
        check("log exists with the g7_cboot_ name, has exit line and stage summary, ECID masked", s.run.logName().matches("g7_cboot_[0-9]{13}\\.txt") && lt.contains("g7-cboot-exit: 0") && !lt.contains(hex) && lt.contains("g7-stage:"));
        check("finished run frees the gate", s.run.finished() && !G7ExecLauncher.hasLiveRun());
        G7CloneBoot.Frame fr = s.run.frame();
        check("the picture is kept in memory (4x2 RGB, class as the runner said), not written to the log", fr != null && fr.w == 4 && fr.h == 2 && fr.rgb.length == 24 && (fr.rgb[0] & 0xff) == 255 && fr.rgb[3] == 0 && fr.cls.equals("mostly-black-small-content") && !lt.contains("ffffff000000") && lt.contains("g7-frame: 4x2 class=mostly-black-small-content"));
        check("verdict keeps 'the machine stopped' apart from 'macOS booted'", s.run.verdict().contains("VM 종료 확인: 예") && s.run.verdict().contains("디스크 flush: 성공") && s.run.verdict().contains("시험 시간 제한") && s.run.verdict().contains("부팅 성공 여부는 판정하지 않았습니다") && lt.contains("g7-cboot-verdict:"));
        for (int um = 1; um <= 2; um++) {
            Fake uf = new Fake("ui_runner_vm_clone RESULT: stopped, confirmed (time limit), ignored input lines 0\n", 0, false);
            G7CloneBoot.Start us = G7CloneBoot.startWith(sp(uf), ROMS, true, () -> false, lib, ext, priv, files, clone, true, none, um);
            check("usb mode " + um + " reaches the command line, installer/ecid flags still absent", us.started && String.join(" ", lastCmd).contains("--usb-mode " + um) && !String.join(" ", lastCmd).contains("--installer")); waitDone(us.run);
        }
        int beforeUsb = spawned;
        check("unknown usb mode 3 / -1: refused, nothing spawned", !G7CloneBoot.startWith(sp(f0), ROMS, true, () -> false, lib, ext, priv, files, clone, true, none, 3).started && !G7CloneBoot.startWith(sp(f0), ROMS, true, () -> false, lib, ext, priv, files, clone, true, none, -1).started && spawned == beforeUsb);
        Fake nof = new Fake("ui_runner_vm_clone FRAME none class=no-frame (read=0)\n", 0, false);
        G7CloneBoot.Start ns = G7CloneBoot.startWith(sp(nof), ROMS, true, () -> false, lib, ext, priv, files, clone, true, none); waitDone(ns.run);
        check("a 'no picture yet' answer is kept as a note the screen can show (not silence)", ns.run.frame() == null && ns.run.frameNote().contains("아직 보낼 화면이 없습니다") && ns.run.frameAnsweredMs > 0);
        Thread.sleep(1100);
        check("malformed frame lines are refused", G7CloneBoot.parseFrame("ui_runner_vm_clone FRAME src=1x1 out=2x2 class=x nonblack_pct=1 colors=1 estimate=pixel-statistics-only data=ff") == null && G7CloneBoot.parseFrame("hello") == null && G7CloneBoot.parseFrame(frameLine.replace("out=4x2", "out=200x2")) == null);
        check("describeAll lists the clone as bootable", G7CloneBoot.describeAll(priv).toString().contains("부팅 가능"));

        Fake crash = new Fake("hengevm: cpu 0 pc sample #1 pc=0x100 el=3\n", 137, false);
        G7CloneBoot.Start cs = G7CloneBoot.startWith(sp(crash), ROMS, true, () -> false, lib, ext, priv, files, clone, true, none); waitDone(cs.run);
        check("a runner that ends without a result line is reported as an abnormal end with unknown cause (not as success)", cs.started && cs.run.exitCode() == 137 && cs.run.verdict().contains("결과 줄을 남기지 않고") && cs.run.verdict().contains("원인 미확인") && !cs.run.verdict().contains("VM 종료 확인: 예"));
        Thread.sleep(1100);
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
