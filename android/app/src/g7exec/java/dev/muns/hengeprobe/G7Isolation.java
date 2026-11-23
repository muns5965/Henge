package dev.muns.hengeprobe;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.List;
import java.util.function.BooleanSupplier;
import java.util.function.IntSupplier;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;

/**
 * [G7-EXEC] Only in the g7exec source set. Puts an existing install pair (macos_disk.img, macos_aux.img, g7_state.txt) aside so that a NEW run can be started, WITHOUT deleting, overwriting, copying or
 * changing the content of anything: the three files are only RENAMED (an atomic rename inside the same directory tree) into vm/archives/g7-archive-NNN/. The 128 GiB sparse disk is never copied.
 *
 * A rename of three files is not one atomic step, so the work is journalled: vm/archives/isolating.txt is created (CREATE_NEW) before the first rename and moved into the archive when everything is checked.
 * While that journal exists the pair is "isolation unfinished": G7ExecLauncher.preflight refuses to start a run (an unfinished isolation is never taken for a clean state). A failed rename is rolled back; if the
 * rollback fails or the app is ended in the middle, the journal stays and recover() finishes the move (only files that are still in vm/ and not yet in the archive are moved; any doubt stops it).
 *
 * Guarantee scope (stated honestly):
 *  - No overwrite: Java cannot ask rename(2) to refuse an existing target (ATOMIC_MOVE replaces silently on Linux), and a hard link + unlink would be a delete. So safeMove checks first, then renames. The check is
 *    race-free against this app because every start and every isolation runs under START_LOCK, the archive folder is created exclusively (mkdir fails if it exists) a moment before, and the only other writer, the
 *    runner process, must be "not found". It is NOT a guarantee against an unrelated process of the same user writing into the app's private folder at that very moment.
 *  - Durability: the journal is forced to storage (FileChannel.force) before the first move and the directories are fsynced (best effort) after each rename. An app kill / crash is covered by the tests (the file
 *    system keeps completed renames). A power loss or a file-system error is NOT proven: the journal may then be missing or older than the moves; recovery stops on any doubt (a file in both places or neither,
 *    wrong size) and a missing journal after an interrupted move shows as a missing pair, never as a clean state that would start a run on its own.
 *  - The runner check is made inside START_LOCK, after the live-run check. It is a read-only /proc look at this user's processes, a helper, not a proof that nothing has the files open.
 *
 * Preconditions of every action: the build's start switch is on (the OFF build changes nothing), no run of this app process is live, the read-only runner check found no runner process (status 0; a found or
 * unknown status stops), and the whole action runs under G7ExecLauncher's START_LOCK so that no start can happen at the same time. No Android classes: tests/java/G7ExecIsolationTest.java runs on a plain JVM.
 */
public final class G7Isolation {
    private G7Isolation() {}

    static final String[] NAMES = {"macos_disk.img", "macos_aux.img", "g7_state.txt"};
    static final String ARCHIVES = "archives";
    static final String JOURNAL = "isolating.txt";

    interface Mover { void move(Path from, Path to) throws IOException; }
    /** A rename inside the same file system. ATOMIC_MOVE never copies (it fails instead). It must not replace anything: safeMove checks that. */
    private static final Mover REAL = (a, b) -> Files.move(a, b, StandardCopyOption.ATOMIC_MOVE);

    public static final class Result {
        public final boolean ok;
        public final String message;
        Result(boolean ok, String message) { this.ok = ok; this.message = message; }
    }

    private static final java.util.regex.Pattern ARCHIVE_NAME = java.util.regex.Pattern.compile("g7-archive-[0-9]{3,}");

    /** True while an isolation was started and not finished (the journal exists). */
    public static boolean pending(File privVmDir) {
        return privVmDir != null && Files.exists(new File(new File(privVmDir, ARCHIVES), JOURNAL).toPath(), LinkOption.NOFOLLOW_LINKS);
    }

    /** How many archive folders exist (for the screen). */
    public static int archiveCount(File privVmDir) {
        File[] l = privVmDir == null ? null : new File(privVmDir, ARCHIVES).listFiles((d, n) -> ARCHIVE_NAME.matcher(n).matches());
        return l == null ? 0 : l.length;
    }

    /** runnerStatus: G7ExecLauncher.runnerStatus (1 found, 0 not found, -1 cannot tell). It is called INSIDE the start lock, after the live-run check, so no start of this app can come between the check and the moves. */
    public static Result isolate(File privVmDir, IntSupplier runnerStatus) {
        return isolateWith(REAL, G7ExecLauncher.START_ENABLED, G7ExecLauncher::hasLiveRun, privVmDir, runnerStatus);
    }

    public static Result recover(File privVmDir, IntSupplier runnerStatus) {
        return recoverWith(REAL, G7ExecLauncher.START_ENABLED, G7ExecLauncher::hasLiveRun, privVmDir, runnerStatus);
    }

    static Result isolateWith(Mover mv, boolean enabled, BooleanSupplier live, File priv, int runnerStatus) { return isolateWith(mv, enabled, live, priv, (IntSupplier) () -> runnerStatus); }
    static Result recoverWith(Mover mv, boolean enabled, BooleanSupplier live, File priv, int runnerStatus) { return recoverWith(mv, enabled, live, priv, (IntSupplier) () -> runnerStatus); }

    private static String gate(boolean enabled, BooleanSupplier live, IntSupplier status) {
        if (!enabled) return "이 빌드의 시작 스위치가 꺼져 있어 격리하지 않았습니다. 아무것도 옮기지 않았습니다.";
        if (live.getAsBoolean()) return "이 앱에서 실행 중인 G7 프로세스가 있어 격리하지 않았습니다.";
        int runnerStatus = status.getAsInt();
        if (runnerStatus == 1) return "실행기 프로세스가 발견되어 격리하지 않았습니다. 아무것도 옮기지 않았습니다.";
        if (runnerStatus != 0) return "실행기 프로세스를 확인할 수 없어 격리하지 않았습니다. 아무것도 옮기지 않았습니다.";
        return null;
    }

    private static boolean regular(Path p) { return Files.isRegularFile(p, LinkOption.NOFOLLOW_LINKS); }
    private static boolean exists(Path p) { return Files.exists(p, LinkOption.NOFOLLOW_LINKS); }

    /** Best effort: ask the file system to write a directory's entries (renames, new names) to storage. Failure is ignored (some systems cannot open a directory); it adds durability, never correctness. */
    private static void syncDir(File dir) {
        try (FileChannel c = FileChannel.open(dir.toPath(), StandardOpenOption.READ)) { c.force(true); } catch (IOException | RuntimeException e) { /* best effort */ }
    }

    /** Creates the journal (CREATE_NEW) and forces its content to storage before returning. */
    private static void writeJournal(Path journal, byte[] data) throws IOException {
        try (FileChannel c = FileChannel.open(journal, StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE)) {
            ByteBuffer b = ByteBuffer.wrap(data);
            while (b.hasRemaining()) c.write(b);
            c.force(true);
        }
    }

    /** Never replaces by itself: a target that exists stops the move (a rename would silently replace it). */
    private static void safeMove(Mover mv, Path from, Path to) throws IOException {
        if (exists(to)) throw new IOException("target exists");
        mv.move(from, to);
        syncDir(from.getParent().toFile()); syncDir(to.getParent().toFile());
    }

    static Result isolateWith(Mover mv, boolean enabled, BooleanSupplier live, File priv, IntSupplier runnerStatus) {
        synchronized (G7ExecLauncher.START_LOCK) {
            String g = gate(enabled, live, runnerStatus);
            if (g != null) return new Result(false, g);
            if (pending(priv)) return new Result(false, "이전 격리가 끝나지 않았습니다. 먼저 복구(마저 옮기기)를 실행하세요. 새로 격리하지 않았습니다.");
            long[] size = new long[NAMES.length];
            StringBuilder missing = new StringBuilder();
            for (int i = 0; i < NAMES.length; i++) {
                Path p = new File(priv, NAMES[i]).toPath();
                if (!regular(p)) { missing.append(' ').append(NAMES[i]); continue; }
                try { size[i] = Files.size(p); } catch (IOException e) { missing.append(' ').append(NAMES[i]); }
            }
            if (missing.length() > 0) return new Result(false, "격리할 세 파일이 모두 일반 파일로 있지 않아 중단했습니다(없거나 링크:" + missing + "). 아무것도 옮기지 않았습니다.");
            File arch = new File(priv, ARCHIVES);
            Path ap = arch.toPath();
            if (exists(ap) && !Files.isDirectory(ap, LinkOption.NOFOLLOW_LINKS)) return new Result(false, "archives가 폴더가 아니어서 중단했습니다. 아무것도 옮기지 않았습니다.");
            if (!exists(ap) && !arch.mkdir()) return new Result(false, "archives 폴더를 만들지 못해 중단했습니다. 아무것도 옮기지 않았습니다.");
            File dest = null;
            for (int n = 1; n < 1000 && dest == null; n++) {
                File c = new File(arch, String.format("g7-archive-%03d", n));
                if (!exists(c.toPath()) && c.mkdir()) dest = c;
            }
            if (dest == null) return new Result(false, "보관 폴더 이름을 정하지 못해 중단했습니다. 아무것도 옮기지 않았습니다.");
            StringBuilder j = new StringBuilder("archive=").append(dest.getName()).append('\n');
            for (int i = 0; i < NAMES.length; i++) j.append("file=").append(NAMES[i]).append(' ').append(size[i]).append('\n');
            Path journal = new File(arch, JOURNAL).toPath();
            syncDir(arch);   // the new archive folder's name
            try { writeJournal(journal, j.toString().getBytes(StandardCharsets.UTF_8)); syncDir(arch); }
            catch (IOException e) { return new Result(false, "격리 기록을 만들지 못해 중단했습니다. 아무것도 옮기지 않았습니다."); }
            int moved = 0;
            try {
                for (; moved < NAMES.length; moved++) safeMove(mv, new File(priv, NAMES[moved]).toPath(), new File(dest, NAMES[moved]).toPath());
            } catch (IOException e) {
                boolean back = true;
                for (int i = moved - 1; i >= 0; i--) {
                    try { safeMove(mv, new File(dest, NAMES[i]).toPath(), new File(priv, NAMES[i]).toPath()); } catch (IOException e2) { back = false; }
                }
                if (back) {
                    try { safeMove(mv, journal, new File(dest, "ROLLED_BACK.txt").toPath()); } catch (IOException e3) { return new Result(false, "일부 이동에 실패해 되돌렸지만 격리 기록을 정리하지 못했습니다. 새 복원은 막혀 있습니다. 복구가 필요합니다."); }
                    return new Result(false, "일부 이동에 실패해 모두 원래 자리로 되돌렸습니다. 설치 쌍은 그대로입니다.");
                }
                return new Result(false, "이동 중 실패했고 일부를 되돌리지 못했습니다. 격리가 끝나지 않은 상태로 남겼으며 새 복원은 막혀 있습니다. 복구(마저 옮기기)를 실행하세요.");
            }
            return finish(mv, priv, dest, journal, size, "ISOLATION_DONE.txt", "격리를 마쳤습니다: " + ARCHIVES + "/" + dest.getName());
        }
    }

    private static Result finish(Mover mv, File priv, File dest, Path journal, long[] size, String doneName, String okMessage) {
        for (int i = 0; i < NAMES.length; i++) {
            Path a = new File(dest, NAMES[i]).toPath();
            boolean ok;
            try { ok = regular(a) && Files.size(a) == size[i] && !exists(new File(priv, NAMES[i]).toPath()); } catch (IOException e) { ok = false; }
            if (!ok) return new Result(false, "이동 뒤 확인이 맞지 않아(" + NAMES[i] + ") 격리를 끝난 것으로 처리하지 않았습니다. 새 복원은 막혀 있습니다. 파일을 확인하세요.");
        }
        try { safeMove(mv, journal, new File(dest, doneName).toPath()); }
        catch (IOException e) { return new Result(false, "세 파일은 옮겨졌지만 격리 기록을 마무리하지 못했습니다. 새 복원은 막혀 있습니다. 복구를 실행하세요."); }
        return new Result(true, okMessage);
    }

    static Result recoverWith(Mover mv, boolean enabled, BooleanSupplier live, File priv, IntSupplier runnerStatus) {
        synchronized (G7ExecLauncher.START_LOCK) {
            String g = gate(enabled, live, runnerStatus);
            if (g != null) return new Result(false, g);
            if (!pending(priv)) return new Result(false, "끝나지 않은 격리가 없습니다.");
            Path journal = new File(new File(priv, ARCHIVES), JOURNAL).toPath();
            List<String> lines;
            try { lines = Files.readAllLines(journal, StandardCharsets.UTF_8); } catch (IOException e) { return new Result(false, "격리 기록을 읽지 못해 중단했습니다. 파일은 그대로입니다."); }
            String archive = null; long[] size = new long[NAMES.length]; boolean[] have = new boolean[NAMES.length];
            for (String l : lines) {
                if (l.startsWith("archive=")) archive = l.substring(8).trim();
                else if (l.startsWith("file=")) {
                    String[] p = l.substring(5).split(" ");
                    for (int i = 0; i < NAMES.length; i++) if (p.length == 2 && p[0].equals(NAMES[i])) { try { size[i] = Long.parseLong(p[1]); have[i] = true; } catch (NumberFormatException e) { /* stays unknown */ } }
                }
            }
            for (boolean h : have) if (!h) return new Result(false, "격리 기록이 올바르지 않아 중단했습니다. 파일은 그대로입니다.");
            if (archive == null || !ARCHIVE_NAME.matcher(archive).matches()) return new Result(false, "격리 기록의 보관 폴더 이름이 올바르지 않아 중단했습니다. 파일은 그대로입니다.");
            File dest = new File(new File(priv, ARCHIVES), archive);
            if (!Files.isDirectory(dest.toPath(), LinkOption.NOFOLLOW_LINKS)) return new Result(false, "보관 폴더가 없어 중단했습니다. 파일은 그대로입니다.");
            // decide every file first; move only when each one is clear (still in vm/ only, or already in the archive only, with the recorded size)
            boolean[] inPriv = new boolean[NAMES.length];
            for (int i = 0; i < NAMES.length; i++) {
                Path a = new File(priv, NAMES[i]).toPath(), b = new File(dest, NAMES[i]).toPath();
                boolean pa = exists(a), pb = exists(b);
                if (pa == pb) return new Result(false, NAMES[i] + (pa ? "가 두 곳에 모두 있어" : "를 어디에서도 찾지 못해") + " 중단했습니다. 수동 확인이 필요합니다. 아무것도 옮기지 않았습니다.");
                Path where = pa ? a : b;
                try { if (!regular(where) || Files.size(where) != size[i]) return new Result(false, NAMES[i] + "의 크기나 종류가 기록과 달라 중단했습니다. 수동 확인이 필요합니다. 아무것도 옮기지 않았습니다."); }
                catch (IOException e) { return new Result(false, NAMES[i] + "를 확인하지 못해 중단했습니다. 아무것도 옮기지 않았습니다."); }
                inPriv[i] = pa;
            }
            for (int i = 0; i < NAMES.length; i++) {
                if (!inPriv[i]) continue;
                try { safeMove(mv, new File(priv, NAMES[i]).toPath(), new File(dest, NAMES[i]).toPath()); }
                catch (IOException e) { return new Result(false, NAMES[i] + "를 옮기지 못했습니다. 격리는 끝나지 않은 상태로 남아 있고 새 복원은 막혀 있습니다."); }
            }
            return finish(mv, priv, dest, journal, size, "ISOLATION_RECOVERED.txt", "끝나지 않은 격리를 마쳤습니다: " + ARCHIVES + "/" + archive);
        }
    }
}
