package dev.muns.hengeprobe;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.function.BooleanSupplier;
import java.util.function.IntSupplier;

/**
 * [G7-EXEC] Only in the g7exec source set. Storage of the install pairs: what is there (free space, the isolated pairs in vm/archives/ with the space they REALLY occupy) and the DELETION of failed pairs,
 * only after the person confirmed it on the screen (this class never decides to delete by itself; nothing calls it automatically).
 *
 * What may be deleted: the members of an install pair (macos_disk.img, macos_aux.img, macos_aux.initial, g7_state.txt) and the isolation's own notes (ISOLATION_DONE.txt, ISOLATION_RECOVERED.txt, ROLLED_BACK.txt),
 * of a pair whose g7_state.txt says "state=in_progress" (a failed or unfinished installation). Never: a pair that is "done" (a finished installation), a pair whose state cannot be read, anything else found in an
 * archive folder (one unknown entry keeps the whole folder), the ROM, the IPSW, the ECID file and the logs (they are not in the pair or in archives/ at all).
 *
 * Preconditions of every deletion (the same as the isolation's: G7Isolation.gate): the build's start switch is on, no run of this app is live, the read-only runner check found no runner process, all inside
 * G7ExecLauncher.START_LOCK so that no start can come in between. An isolation that was not finished (journal) blocks every deletion: the pair named in it is half moved.
 *
 * Order of deletion inside one pair: disk, AUX, AUX copy, notes, and the state file LAST, then the (empty) folder. An app end or an error in the middle leaves a pair that still says "in_progress" (so it can be
 * deleted again and the deletion simply continues) and never a disk without its state file. A failure stops that pair, is reported with what was and what was not deleted, and nothing else is touched.
 * No Android classes: tests/java/G7ExecStorageTest.java runs on a plain JVM.
 */
public final class G7Storage {
    private G7Storage() {}

    /** The pair's own files, in the order they are deleted (the state file is the last). */
    static final String[] DELETE_ORDER = {"macos_disk.img", "macos_aux.img", "macos_aux.initial", "ISOLATION_DONE.txt", "ISOLATION_RECOVERED.txt", "ROLLED_BACK.txt", "g7_state.txt"};
    /** An estimate, not a measured need: the least free space the person must have before a new restore is started (the sparse disk grows while it installs; a failed run was seen at ~12 GB). */
    public static final long MIN_FREE_BYTES = 30L << 30;
    private static final java.util.regex.Pattern ARCHIVE_NAME = java.util.regex.Pattern.compile("g7-archive-[0-9]{3,}");

    /** The space a file really occupies on the storage (allocated blocks), not its logical size: the 128 GiB disk is sparse. The app gives android.system.Os.stat(..).st_blocks * 512. */
    public interface Sizer { long allocated(File f) throws IOException; }
    interface Deleter { void remove(Path p) throws IOException; }
    private static final Deleter REAL = Files::delete;

    public static final class Item {
        public final String name; public final long createdMs; public final long allocated; public final String state; public final boolean deletable; public final String note;
        Item(String name, long createdMs, long allocated, String state, boolean deletable, String note) { this.name = name; this.createdMs = createdMs; this.allocated = allocated; this.state = state; this.deletable = deletable; this.note = note; }
    }

    public static final class Result {
        public final boolean ok; public final String message;
        Result(boolean ok, String message) { this.ok = ok; this.message = message; }
    }

    private static boolean regular(Path p) { return Files.isRegularFile(p, LinkOption.NOFOLLOW_LINKS); }
    private static boolean exists(Path p) { return Files.exists(p, LinkOption.NOFOLLOW_LINKS); }

    /** "in_progress", "done", or "unknown" (missing, unreadable, anything else). Only the first line of the state file is looked at. */
    static String stateOf(File stateFile) {
        try {
            if (!regular(stateFile.toPath())) return "unknown";
            byte[] b = Files.readAllBytes(stateFile.toPath());
            String first = new String(b, 0, Math.min(b.length, 200), StandardCharsets.UTF_8).split("\n", 2)[0].trim();
            return first.equals("state=in_progress") ? "in_progress" : first.equals("state=done") ? "done" : "unknown";
        } catch (IOException | RuntimeException e) { return "unknown"; }
    }

    private static boolean known(String n) { for (String k : DELETE_ORDER) if (k.equals(n)) return true; return false; }

    /** Free space for the screen and for the check before a start. */
    public static String fmt(long bytes) {
        if (bytes < 0) return "?";
        double g = bytes / (double) (1L << 30);
        return g >= 0.1 ? String.format(java.util.Locale.US, "%.1f GB", g) : String.format(java.util.Locale.US, "%.1f MB", bytes / (double) (1L << 20));
    }

    /** "" when there is enough, else the reason (free space is read at the moment of the call). */
    public static String enoughFor(long freeBytes) {
        if (freeBytes >= MIN_FREE_BYTES) return "";
        return "저장공간이 부족해 시작하지 않았습니다: 사용 가능 " + fmt(freeBytes) + ", 필요 최소 " + fmt(MIN_FREE_BYTES) + "(추정값). 실패한 설치 파일을 삭제해 공간을 확보하세요.";
    }

    /** The archive folders (newest name last) with their real size and what may be done with them. Reads only. */
    public static List<Item> list(File priv, Sizer sizer) {
        List<Item> out = new ArrayList<>();
        File[] dirs = priv == null ? null : new File(priv, G7Isolation.ARCHIVES).listFiles((d, n) -> ARCHIVE_NAME.matcher(n).matches());
        if (dirs == null) return out;
        java.util.Arrays.sort(dirs);
        for (File d : dirs) out.add(inspect(d, sizer));
        return out;
    }

    private static Item inspect(File d, Sizer sizer) {
        if (!Files.isDirectory(d.toPath(), LinkOption.NOFOLLOW_LINKS)) return new Item(d.getName(), 0, 0, "unknown", false, "폴더가 아님");
        File[] fs = d.listFiles();
        if (fs == null) return new Item(d.getName(), 0, 0, "unknown", false, "읽을 수 없음");
        long alloc = 0, created = d.lastModified(); String unknown = null; boolean payload = false;
        for (File f : fs) {
            if (!known(f.getName()) || !regular(f.toPath())) { unknown = f.getName(); continue; }
            if (f.getName().startsWith("macos_")) payload = true;
            try { alloc += sizer.allocated(f); } catch (IOException | RuntimeException e) { /* shown as less than it is; never blocks the list */ }
            if (f.getName().equals("ISOLATION_DONE.txt") || f.getName().equals("ISOLATION_RECOVERED.txt")) created = f.lastModified();
        }
        File st = new File(d, "g7_state.txt");
        String state = exists(st.toPath()) ? stateOf(st) : "unknown";
        boolean leftover = !exists(st.toPath()) && !payload;   // only notes left of a deletion that was interrupted
        if (unknown != null) return new Item(d.getName(), created, alloc, state, false, "알 수 없는 항목(" + unknown + ")이 있어 삭제 대상에서 제외");
        if (state.equals("done")) return new Item(d.getName(), created, alloc, state, false, "정상 설치가 끝난 디스크라 삭제 대상에서 제외");
        if (state.equals("unknown") && !leftover) return new Item(d.getName(), created, alloc, state, false, "상태를 확인할 수 없어 삭제 대상에서 제외");
        return new Item(d.getName(), created, alloc, leftover ? "in_progress" : state, true, "");
    }

    /** Deletes the chosen archive folders (after the person confirmed). Names are checked against the folder-name pattern; each folder is judged again here, not by what the list showed earlier. */
    public static Result deleteArchives(File priv, List<String> names, IntSupplier runnerStatus, Sizer sizer) {
        return deleteArchivesWith(REAL, G7ExecLauncher.START_ENABLED, G7ExecLauncher::hasLiveRun, priv, names, runnerStatus, sizer);
    }

    static Result deleteArchivesWith(Deleter del, boolean enabled, BooleanSupplier live, File priv, List<String> names, IntSupplier runnerStatus, Sizer sizer) {
        synchronized (G7ExecLauncher.START_LOCK) {
            String g = G7Isolation.gate(enabled, live, runnerStatus);
            if (g != null) return new Result(false, g.replace("격리하지", "삭제하지").replace("아무것도 옮기지", "아무것도 지우지"));
            if (G7Isolation.pending(priv)) return new Result(false, "끝나지 않은 격리가 있어 삭제하지 않았습니다. 먼저 복구를 실행하세요. 아무것도 지우지 않았습니다.");
            if (names == null || names.isEmpty()) return new Result(false, "삭제할 보관 폴더가 선택되지 않았습니다.");
            StringBuilder msg = new StringBuilder(); boolean all = true; int deleted = 0; long freed = 0;
            List<String> sorted = new ArrayList<>(names); Collections.sort(sorted);
            for (String n : sorted) {
                if (n == null || !ARCHIVE_NAME.matcher(n).matches()) { msg.append("\n- ").append(n).append(": 올바른 보관 폴더 이름이 아니라 건너뜀"); all = false; continue; }
                File d = new File(new File(priv, G7Isolation.ARCHIVES), n);
                if (!exists(d.toPath())) { msg.append("\n- ").append(n).append(": 이미 없음"); continue; }
                Item it = inspect(d, sizer);
                if (!it.deletable) { msg.append("\n- ").append(n).append(": 삭제하지 않음 — ").append(it.note); all = false; continue; }
                long before = it.allocated;
                String r = deleteMembers(del, d, true);
                if (r == null) { deleted++; freed += before; msg.append("\n- ").append(n).append(": 삭제함"); }
                else { all = false; msg.append("\n- ").append(n).append(": ").append(r); }
            }
            return new Result(all, (all ? "삭제를 마쳤습니다(" + deleted + "개 폴더)." : "일부만 처리했습니다(삭제한 폴더 " + deleted + "개).") + msg);
        }
    }

    /** The install pair that is still in vm/ (a failed run that was not isolated): deleted directly, only when it is "in_progress". A finished ("done") pair is never deleted here. */
    public static Result deleteCurrent(File priv, IntSupplier runnerStatus) {
        return deleteCurrentWith(REAL, G7ExecLauncher.START_ENABLED, G7ExecLauncher::hasLiveRun, priv, runnerStatus);
    }

    static Result deleteCurrentWith(Deleter del, boolean enabled, BooleanSupplier live, File priv, IntSupplier runnerStatus) {
        synchronized (G7ExecLauncher.START_LOCK) {
            String g = G7Isolation.gate(enabled, live, runnerStatus);
            if (g != null) return new Result(false, g.replace("격리하지", "삭제하지").replace("아무것도 옮기지", "아무것도 지우지"));
            if (G7Isolation.pending(priv)) return new Result(false, "끝나지 않은 격리가 있어 삭제하지 않았습니다. 먼저 복구를 실행하세요. 아무것도 지우지 않았습니다.");
            boolean any = false;
            for (String n : G7Isolation.NAMES) {
                Path p = new File(priv, n).toPath();
                if (!exists(p)) continue;
                any = true;
                if (!regular(p)) return new Result(false, n + "가 일반 파일이 아니어서(링크 등) 삭제하지 않았습니다. 아무것도 지우지 않았습니다.");
            }
            if (!any) return new Result(false, "삭제할 설치 쌍 파일이 없습니다.");
            String state = stateOf(new File(priv, "g7_state.txt"));
            if (state.equals("done")) return new Result(false, "정상 설치가 끝난 디스크(state=done)라 삭제하지 않았습니다. 아무것도 지우지 않았습니다.");
            if (!state.equals("in_progress")) return new Result(false, "설치 쌍의 상태를 확인할 수 없어(state=in_progress가 아님) 삭제하지 않았습니다. 아무것도 지우지 않았습니다.");
            String r = deleteMembers(del, priv, false);
            return r == null ? new Result(true, "실패한 설치 쌍을 삭제했습니다. 새 복원을 시작할 수 있습니다.") : new Result(false, r);
        }
    }

    private static final java.util.regex.Pattern CLONE_NAME = java.util.regex.Pattern.compile("clone-[0-9]{3,}");

    /** One folder under vm/clones/ for the delete list: its name and the space it really occupies. Looks only. */
    public static List<Item> listClones(File priv, Sizer sizer) {
        List<Item> out = new ArrayList<>();
        File[] k = new File(priv, "clones").listFiles();
        if (k == null) return out;
        Arrays.sort(k);
        for (File d : k) {
            if (!CLONE_NAME.matcher(d.getName()).matches() || !Files.isDirectory(d.toPath(), LinkOption.NOFOLLOW_LINKS)) continue;
            long alloc = 0; File[] in = d.listFiles();
            if (in != null) for (File f : in) { try { alloc += sizer.allocated(f); } catch (IOException | RuntimeException e) { /* shown as less than it is */ } }
            out.add(new Item(d.getName(), d.lastModified(), alloc, "clone", true, ""));
        }
        return out;
    }

    /**
     * Deletes ONE clone folder vm/clones/clone-NNN (after the person confirmed). The install pair in vm/ and every other clone are never touched. Refused while any install run, clone boot or clone copy is live
     * (a boot or a copy may be reading or writing exactly this folder). The marker clone_state.txt goes FIRST: from that moment the folder is no clone (it cannot be booted or picked as a source), so an interruption leaves
     * a harmless leftover that can be deleted again. Only regular files directly inside the folder are removed; anything else (a sub-folder, a link) stops the deletion with the rest untouched.
     */
    public static Result deleteClone(File priv, String name, IntSupplier runnerStatus, BooleanSupplier otherLive) {
        return deleteCloneWith(REAL, G7ExecLauncher.START_ENABLED, G7ExecLauncher::hasLiveRun, otherLive, priv, name, runnerStatus);
    }

    static Result deleteCloneWith(Deleter del, boolean enabled, BooleanSupplier live, BooleanSupplier otherLive, File priv, String name, IntSupplier runnerStatus) {
        synchronized (G7ExecLauncher.START_LOCK) {
            String g = G7Isolation.gate(enabled, live, runnerStatus);
            if (g != null) return new Result(false, g.replace("격리하지", "삭제하지").replace("아무것도 옮기지", "아무것도 지우지"));
            if (otherLive != null && otherLive.getAsBoolean()) return new Result(false, "복제본 부팅 또는 복제가 진행 중이라 삭제하지 않았습니다. 아무것도 지우지 않았습니다.");
            if (name == null || !CLONE_NAME.matcher(name).matches()) return new Result(false, "올바른 복제본 이름이 아닙니다. 아무것도 지우지 않았습니다.");
            File clones = new File(priv, "clones"), d = new File(clones, name);
            if (!Files.isDirectory(clones.toPath(), LinkOption.NOFOLLOW_LINKS)) return new Result(false, "복제본 폴더가 없습니다.");
            if (!exists(d.toPath())) return new Result(false, name + "은(는) 이미 없습니다.");
            if (!Files.isDirectory(d.toPath(), LinkOption.NOFOLLOW_LINKS)) return new Result(false, name + "이(가) 폴더가 아니어서 삭제하지 않았습니다. 아무것도 지우지 않았습니다.");
            File[] in = d.listFiles();
            if (in == null) return new Result(false, name + " 안을 읽지 못해 삭제하지 않았습니다. 아무것도 지우지 않았습니다.");
            for (File f : in) if (!regular(f.toPath())) return new Result(false, name + " 안의 " + f.getName() + "이(가) 일반 파일이 아니어서(폴더·링크 등) 삭제하지 않았습니다. 아무것도 지우지 않았습니다.");
            List<File> order = new ArrayList<>(Arrays.asList(in));
            order.sort((a, b) -> Integer.compare(rank(a.getName()), rank(b.getName())));
            int n = 0;
            for (File f : order) {
                try { del.remove(f.toPath()); n++; }
                catch (IOException e) { return new Result(false, f.getName() + "을(를) 지우지 못해 멈췄습니다(" + n + "개 삭제됨). 이 폴더는 이미 복제본으로 쓸 수 없고, 다시 삭제하면 이어서 지웁니다."); }
            }
            try { del.remove(d.toPath()); }
            catch (IOException e) { return new Result(false, "파일은 지웠지만 빈 폴더를 지우지 못했습니다(다시 삭제하면 정리됩니다)."); }
            return new Result(true, name + "을(를) 삭제했습니다(파일 " + n + "개). 설치 쌍과 다른 복제본은 그대로입니다.");
        }
    }

    /** Marker first (no longer a clone), then the big files, then the rest. */
    private static int rank(String n) { return n.equals("clone_state.txt") ? 0 : n.startsWith("macos_disk") ? 1 : n.startsWith("macos_") ? 2 : 3; }

    /** Deletes the pair's files in DELETE_ORDER (those that exist) and, for an archive folder, the empty folder. Null when everything is gone; else what happened. Never touches an entry outside DELETE_ORDER. */
    private static String deleteMembers(Deleter del, File dir, boolean removeDir) {
        int n = 0;
        for (String name : DELETE_ORDER) {
            Path p = new File(dir, name).toPath();
            if (!exists(p)) continue;
            if (!regular(p)) return name + "가 일반 파일이 아니어서 멈췄습니다(" + n + "개 삭제됨). 나머지는 그대로입니다.";
            if (!removeDir && name.endsWith(".txt") && !name.equals("g7_state.txt")) continue;   // the isolation's notes exist only inside archive folders
            try { del.remove(p); n++; }
            catch (IOException e) { return name + "를 삭제하지 못해 멈췄습니다(" + n + "개 삭제됨, 나머지는 그대로이며 상태 파일이 남아 있으면 다시 삭제할 수 있습니다)."; }
        }
        if (removeDir) {
            try { del.remove(dir.toPath()); }
            catch (IOException e) { return "파일은 지웠지만 빈 폴더를 지우지 못했습니다(다시 삭제하면 정리됩니다)."; }
        }
        return null;
    }
}
