/* host/clone_dir.h on this machine: what makes a folder a finished clone of THIS vm folder and ECID, the files being kept open, and the ECID read. Plain Linux, temporary directories only. */
#define _GNU_SOURCE
#include "clone_dir.h"
#include <sys/types.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } else { printf("PASS "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define ECID 0x12345678901ull

static void wr(const char *dir, const char *name, const char *text, size_t pad) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w"); fputs(text, f); if (pad) { fseek(f, (long)pad - 1, SEEK_SET); fputc(0, f); } fclose(f);
}
static void good_with(const char *dir, const char *tag, const char *members) {
    char t[400];
    wr(dir, "macos_disk.img", "", 1 << 20); wr(dir, "macos_aux.img", "x", 0); wr(dir, "macos_aux.initial", "y", 0);
    snprintf(t, sizeof t, "state=in_progress\necid_tag=%s\n", tag); wr(dir, "g7_state.txt", t, 0);
    snprintf(t, sizeof t, "state=clone_complete\nsource_state=in_progress\nlogical_disk_bytes=1048576\nallocated_bytes=1\nmembers=%s\necid_tag=%s\n", members, tag); wr(dir, "clone_state.txt", t, 0);
}
static void good(const char *dir) { char tag[17]; hv_clone_ecid_tag(ECID, tag); good_with(dir, tag, "3"); }
static int open_clone(const char *dir, const char *vm, uint64_t ecid, HvClone *c, char *why, size_t wc) { return hv_clone_open(dir, vm, ecid, c, why, wc); }
#define REFUSED(dir, ...) (open_clone(dir, root, ECID, &c, why, sizeof why) != 0)

int main(void) {
    char root[PATH_MAX]; snprintf(root, sizeof root, "/tmp/clonedir-test-%d", (int)getpid());
    char clones[PATH_MAX], dir[PATH_MAX], why[200], tag[17]; HvClone c;
    mkdir(root, 0700); snprintf(clones, sizeof clones, "%s/clones", root); mkdir(clones, 0700);
    wr(root, "macos_disk.img", "orig", 0); wr(root, "macos_aux.img", "orig", 0);
    hv_clone_ecid_tag(ECID, tag);
    CHECK(!strcmp(tag, "b15a69182ce0d9c4"), "the tag matches the Java/C reference vector (%s)", tag);
    snprintf(dir, sizeof dir, "%s/clone-001", clones); mkdir(dir, 0700); good(dir);
    CHECK(open_clone(dir, root, ECID, &c, why, sizeof why) == 0 && !strncmp(c.disk, "/proc/self/fd/", 14) && strstr(c.real, "/clone-001"), "a finished clone of this folder and ECID is accepted: %s", why);
    CHECK(c.disk_fd >= 0 && access(c.disk, R_OK) == 0, "the machine would get a /proc/self/fd path of the file that was checked");
    HvClone c2; CHECK(open_clone(dir, root, ECID, &c2, why, sizeof why) != 0 && strstr(why, "in use"), "a second runner on the same clone is refused: %s", why);
    hv_clone_close(&c);
    CHECK(open_clone(dir, root, ECID, &c, why, sizeof why) == 0, "after closing, the clone can be opened again (the lock went with the descriptors)"); hv_clone_close(&c);
    CHECK(open_clone(dir, root, ECID + 1, &c, why, sizeof why) != 0 && strstr(why, "another machine identity"), "another ECID is refused: %s", why);
    CHECK(open_clone(dir, root, 0, &c, why, sizeof why) != 0, "no ECID is refused");
    CHECK(open_clone("clones/clone-001", root, ECID, &c, why, sizeof why) != 0, "a relative path is refused");
    char odd[PATH_MAX]; snprintf(odd, sizeof odd, "%s/../clones/clone-001", clones);
    CHECK(REFUSED(odd), "a path with .. is refused: %s", why);
    char other[PATH_MAX]; snprintf(other, sizeof other, "%s/vm", root); mkdir(other, 0700); good(other);
    CHECK(REFUSED(other), "the original pair folder (not clones/clone-NNN) is refused: %s", why);
    char foreign[PATH_MAX]; snprintf(foreign, sizeof foreign, "/tmp/clonedir-foreign-%d", (int)getpid()); mkdir(foreign, 0700);
    char fclones[PATH_MAX], fdir[PATH_MAX]; snprintf(fclones, sizeof fclones, "%s/clones", foreign); mkdir(fclones, 0700); snprintf(fdir, sizeof fdir, "%s/clone-001", fclones); mkdir(fdir, 0700); good(fdir);
    CHECK(REFUSED(fdir), "a clone of ANOTHER vm folder is refused: %s", why);
    char b[PATH_MAX]; int k = 2;
#define MK(name) snprintf(b, sizeof b, "%s/clone-%03d", clones, k++); mkdir(b, 0700); good(b)
    MK(); snprintf(odd, sizeof odd, "%s/clone_state.txt", b); remove(odd);
    CHECK(REFUSED(b) && strstr(why, "marker"), "no marker: not a finished clone: %s", why);
    MK(); wr(b, "clone_state.txt", "state=clone_complete\nlogical_disk_bytes=5\n", 0);
    CHECK(REFUSED(b), "a short marker is refused: %s", why);
    MK(); { char t[400]; snprintf(t, sizeof t, "state=clone_complete\nsource_state=in_progress\nlogical_disk_bytes=5\nallocated_bytes=1\nmembers=3\necid_tag=%s\n", tag); wr(b, "clone_state.txt", t, 0); }
    CHECK(REFUSED(b) && strstr(why, "size"), "a marker whose size does not match: %s", why);
    MK(); { char t[400]; snprintf(t, sizeof t, "state=clone_complete\nsource_state=in_progress\nlogical_disk_bytes=1048576\nallocated_bytes=1\nmembers=2\necid_tag=%s\n", tag); wr(b, "clone_state.txt", t, 0); }
    CHECK(REFUSED(b) && strstr(why, "member"), "a marker whose member count does not match the files: %s", why);
    MK(); { char t[400]; snprintf(t, sizeof t, "state=clone_complete\nsource_state=in_progress\nlogical_disk_bytes=1048576\nallocated_bytes=1\nmembers=3\necid_tag=%s\nextra=1\n", tag); wr(b, "clone_state.txt", t, 0); }
    CHECK(REFUSED(b), "a marker with an extra line is refused: %s", why);
    MK(); { char t[400]; snprintf(t, sizeof t, "state=clone_complete\nsource_state=weird\nlogical_disk_bytes=1048576\nallocated_bytes=1\nmembers=3\necid_tag=%s\n", tag); wr(b, "clone_state.txt", t, 0); }
    CHECK(REFUSED(b), "an unknown source state is refused: %s", why);
    MK(); good_with(b, "0123456789abcdef", "3");
    CHECK(REFUSED(b) && strstr(why, "another machine identity"), "files tied to another ECID tag are refused: %s", why);
    MK(); wr(b, "g7_state.txt", "state=in_progress\necid_tag=0123456789abcdef\n", 0);
    CHECK(REFUSED(b) && strstr(why, "another machine identity"), "g7_state.txt with another tag (marker has the right one) is refused: %s", why);
    MK(); wr(b, "g7_state.txt", "state=in_progress\n", 0);
    CHECK(REFUSED(b) && strstr(why, "ECID"), "no ecid_tag line: not tied to an ECID: %s", why);
    MK(); wr(b, "macos_disk.img.partial", "x", 0);
    CHECK(REFUSED(b) && strstr(why, "does not belong"), "a .partial left: %s", why);
    MK(); wr(b, "notes.txt", "x", 0);
    CHECK(REFUSED(b), "an unknown file is refused: %s", why);
    MK(); { char tgt[PATH_MAX], lnk[PATH_MAX]; snprintf(tgt, sizeof tgt, "%s/macos_disk.img", root); snprintf(lnk, sizeof lnk, "%s/macos_disk.img", b); remove(lnk); symlink(tgt, lnk); }
    CHECK(REFUSED(b), "a disk that is a symbolic link to the original is refused: %s", why);
    MK(); { char tgt[PATH_MAX], lnk[PATH_MAX]; snprintf(lnk, sizeof lnk, "%s/macos_aux.img", b); snprintf(tgt, sizeof tgt, "%s/macos_aux.img", root); remove(lnk); link(tgt, lnk); }
    CHECK(REFUSED(b), "an AUX that is a hard link to the original is refused: %s", why);
    char viadir[PATH_MAX]; snprintf(viadir, sizeof viadir, "%s/clone-090", clones); symlink(b, viadir);
    CHECK(REFUSED(viadir), "the clone folder itself as a symbolic link is refused: %s", why);
    MK(); wr(b, "macos_aux.initial", "", 0); { char p2[PATH_MAX]; snprintf(p2, sizeof p2, "%s/macos_aux.initial", b); remove(p2); symlink("/etc/hostname", p2); }
    CHECK(REFUSED(b), "an AUX.initial that is a link is refused: %s", why);
    /* a swap after the check does not change what the machine is given */
    MK(); CHECK(open_clone(b, root, ECID, &c, why, sizeof why) == 0, "setup for the swap test");
    { char dpath[PATH_MAX]; snprintf(dpath, sizeof dpath, "%s/macos_disk.img", b); remove(dpath); symlink(root, dpath); }
    { char cur[PATH_MAX]; ssize_t n = readlink(c.disk, cur, sizeof cur - 1); if (n > 0) cur[n] = 0; else cur[0] = 0;
      CHECK(strstr(cur, "macos_disk.img") != NULL && strstr(cur, "(deleted)") != NULL, "after the swap, the held descriptor still points at the file that was checked (now unlinked): %s", cur); }
    hv_clone_close(&c);

    /* ---- direct boot of the restored original pair ---- */
    {
        char dv[PATH_MAX], st[200]; snprintf(dv, sizeof dv, "%s/direct", root); mkdir(dv, 0700);
        wr(dv, "macos_disk.img", "", 1 << 20); wr(dv, "macos_aux.img", "x", 0);
        snprintf(st, sizeof st, "state=done\necid_tag=%s\n", tag); wr(dv, "g7_state.txt", st, 0);
        wr(dv, "vm_ecid.txt", "ecid=0x12345678901\n", 0); wr(dv, "some_other_file.bin", "z", 0);   /* the VM folder holds other things: they are not listed or touched */
        HvClone d1, d2;
        CHECK(hv_direct_open(dv, ECID, &d1, why, sizeof why) == 0 && !strncmp(d1.disk, "/proc/self/fd/", 14) && !strcmp(d1.real, dv), "direct: a pair with state=done and this ECID's tag is accepted: %s", why);
        CHECK(hv_direct_open(dv, ECID, &d2, why, sizeof why) != 0 && strstr(why, "in use"), "direct: a second runner on the same pair is refused: %s", why);
        hv_clone_close(&d1);
        CHECK(hv_direct_open(dv, ECID, &d1, why, sizeof why) == 0, "direct: after closing, the pair can be opened again"); hv_clone_close(&d1);
        CHECK(hv_direct_open(dv, ECID + 1, &d1, why, sizeof why) != 0 && strstr(why, "another machine identity"), "direct: another ECID is refused: %s", why);
        CHECK(hv_direct_open(dv, 0, &d1, why, sizeof why) != 0, "direct: no ECID is refused");
        CHECK(hv_direct_open("direct", ECID, &d1, why, sizeof why) != 0, "direct: a relative path is refused");
        snprintf(odd, sizeof odd, "%s/../direct", clones); CHECK(hv_direct_open(odd, ECID, &d1, why, sizeof why) != 0, "direct: a path with .. is refused: %s", why);
        snprintf(st, sizeof st, "state=in_progress\necid_tag=%s\n", tag); wr(dv, "g7_state.txt", st, 0);
        CHECK(hv_direct_open(dv, ECID, &d1, why, sizeof why) != 0 && strstr(why, "not marked done"), "direct: an unfinished restore (in_progress) is never booted: %s", why);
        wr(dv, "g7_state.txt", "state=done\n", 0);
        CHECK(hv_direct_open(dv, ECID, &d1, why, sizeof why) != 0 && strstr(why, "ecid_tag"), "direct: state=done without a tag is refused: %s", why);
        snprintf(st, sizeof st, "state=done\necid_tag=0000000000000000\n"); wr(dv, "g7_state.txt", st, 0);
        CHECK(hv_direct_open(dv, ECID, &d1, why, sizeof why) != 0 && strstr(why, "another machine identity"), "direct: a pair tied to another machine is refused: %s", why);
        snprintf(st, sizeof st, "state=done\necid_tag=%s\n", tag); wr(dv, "g7_state.txt", st, 0);
        char lp[PATH_MAX], tg[PATH_MAX]; snprintf(lp, sizeof lp, "%s/macos_aux.img", dv); snprintf(tg, sizeof tg, "%s/aux_target", dv); rename(lp, tg); if (symlink(tg, lp)) {}
        CHECK(hv_direct_open(dv, ECID, &d1, why, sizeof why) != 0, "direct: a linked AUX is refused: %s", why);
        remove(lp); rename(tg, lp);
        snprintf(tg, sizeof tg, "%s/aux_hard", dv); if (link(lp, tg)) {}
        CHECK(hv_direct_open(dv, ECID, &d1, why, sizeof why) != 0 && strstr(why, "another link"), "direct: a hard-linked AUX is refused: %s", why);
        remove(tg);
        snprintf(lp, sizeof lp, "%s/macos_disk.img", dv); remove(lp);
        CHECK(hv_direct_open(dv, ECID, &d1, why, sizeof why) != 0 && strstr(why, "missing"), "direct: a missing disk is refused: %s", why);
        CHECK(hv_direct_open(dv, ECID, &d1, why, sizeof why) != 0 && d1.disk_fd < 0 && d1.aux_fd < 0 && d1.dirfd < 0, "direct: a refusal leaves nothing open");
    }
    wr(root, "vm_ecid.txt", "ecid=0x12345678901\n", 0);
    CHECK(hv_clone_read_ecid(root) == ECID, "a valid ECID file is read");
    wr(root, "vm_ecid.txt", "ecid=0x1\n", 0); CHECK(hv_clone_read_ecid(root) == 0, "a value below 2^40 is refused");
    wr(root, "vm_ecid.txt", "ecid=0xffffffffffffffff\n", 0); CHECK(hv_clone_read_ecid(root) == 0, "a value above 2^62 is refused");
    wr(root, "vm_ecid.txt", "ecid=0x12zz\n", 0); CHECK(hv_clone_read_ecid(root) == 0, "garbage is refused");
    CHECK(hv_clone_read_ecid("/tmp/no-such-dir-clonedir") == 0, "a missing file is 0");
    char cmd[PATH_MAX * 2 + 40]; snprintf(cmd, sizeof cmd, "rm -rf %s %s", root, foreign); if (system(cmd)) {}
    printf(g_fail ? "CLONEDIR FAILED (%d)\n" : "CLONEDIR OK\n", g_fail);
    return g_fail != 0;
}
