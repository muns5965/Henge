/* host/clone_dir.h on this machine: what makes a folder a finished clone, and the ECID read. Plain Linux, temporary directories only. */
#define _GNU_SOURCE
#include "clone_dir.h"
#include <sys/types.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } else { printf("PASS "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void wr(const char *dir, const char *name, const char *text, size_t pad) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w"); fputs(text, f); if (pad) { fseek(f, (long)pad - 1, SEEK_SET); fputc(0, f); } fclose(f);
}
static void good(const char *dir) {
    wr(dir, "macos_disk.img", "", 1 << 20); wr(dir, "macos_aux.img", "x", 0);
    wr(dir, "g7_state.txt", "state=in_progress\necid_tag=0123456789abcdef\n", 0);
    wr(dir, "clone_state.txt", "state=clone_complete\nsource_state=in_progress\nlogical_disk_bytes=1048576\nallocated_bytes=1\nmembers=3\n", 0);
}
int main(void) {
    char root[PATH_MAX]; snprintf(root, sizeof root, "/tmp/clonedir-test-%d", (int)getpid());
    char clones[PATH_MAX], dir[PATH_MAX], disk[PATH_MAX], aux[PATH_MAX], why[200];
    mkdir(root, 0700); snprintf(clones, sizeof clones, "%s/clones", root); mkdir(clones, 0700);
    snprintf(dir, sizeof dir, "%s/clone-001", clones); mkdir(dir, 0700); good(dir);
    CHECK(hv_clone_dir_ok(dir, disk, sizeof disk, aux, sizeof aux, why, sizeof why) == 0 && strstr(disk, "/clone-001/macos_disk.img"), "a finished clone is accepted, the paths are built from the folder");
    CHECK(hv_clone_dir_ok("clones/clone-001", disk, sizeof disk, aux, sizeof aux, why, sizeof why) != 0, "a relative path is refused");
    char odd[PATH_MAX]; snprintf(odd, sizeof odd, "%s/../clones/clone-001", clones);
    CHECK(hv_clone_dir_ok(odd, disk, sizeof disk, aux, sizeof aux, why, sizeof why) != 0, "a path with .. is refused: %s", why);
    char other[PATH_MAX]; snprintf(other, sizeof other, "%s/vm", root); mkdir(other, 0700); good(other);
    CHECK(hv_clone_dir_ok(other, disk, sizeof disk, aux, sizeof aux, why, sizeof why) != 0, "the original pair folder (not clones/clone-NNN) is refused: %s", why);
    char bad1[PATH_MAX]; snprintf(bad1, sizeof bad1, "%s/clone-002", clones); mkdir(bad1, 0700); good(bad1); remove((snprintf(odd, sizeof odd, "%s/clone_state.txt", bad1), odd));
    CHECK(hv_clone_dir_ok(bad1, disk, sizeof disk, aux, sizeof aux, why, sizeof why) != 0 && strstr(why, "marker"), "no marker: not a finished clone: %s", why);
    char bad2[PATH_MAX]; snprintf(bad2, sizeof bad2, "%s/clone-003", clones); mkdir(bad2, 0700); good(bad2); wr(bad2, "clone_state.txt", "state=clone_complete\nlogical_disk_bytes=5\n", 0);
    CHECK(hv_clone_dir_ok(bad2, disk, sizeof disk, aux, sizeof aux, why, sizeof why) != 0 && strstr(why, "size"), "a marker whose size does not match: %s", why);
    char bad3[PATH_MAX]; snprintf(bad3, sizeof bad3, "%s/clone-004", clones); mkdir(bad3, 0700); good(bad3); wr(bad3, "macos_disk.img.partial", "x", 0);
    CHECK(hv_clone_dir_ok(bad3, disk, sizeof disk, aux, sizeof aux, why, sizeof why) != 0 && strstr(why, "partial"), "a .partial left: %s", why);
    char bad4[PATH_MAX]; snprintf(bad4, sizeof bad4, "%s/clone-005", clones); mkdir(bad4, 0700); good(bad4); wr(bad4, "g7_state.txt", "state=in_progress\n", 0);
    CHECK(hv_clone_dir_ok(bad4, disk, sizeof disk, aux, sizeof aux, why, sizeof why) != 0 && strstr(why, "ECID"), "no ecid_tag: not tied to an ECID: %s", why);
    char bad5[PATH_MAX]; snprintf(bad5, sizeof bad5, "%s/clone-006", clones); mkdir(bad5, 0700); good(bad5);
    char tgt[PATH_MAX]; snprintf(tgt, sizeof tgt, "%s/macos_disk.img", other); char lnk[PATH_MAX]; snprintf(lnk, sizeof lnk, "%s/macos_disk.img", bad5); remove(lnk); symlink(tgt, lnk);
    CHECK(hv_clone_dir_ok(bad5, disk, sizeof disk, aux, sizeof aux, why, sizeof why) != 0, "a disk that is a symbolic link to the original is refused: %s", why);
    char bad6[PATH_MAX]; snprintf(bad6, sizeof bad6, "%s/clone-007", clones); mkdir(bad6, 0700); good(bad6); snprintf(lnk, sizeof lnk, "%s/macos_aux.img", bad6); remove(lnk); snprintf(tgt, sizeof tgt, "%s/macos_aux.img", other); link(tgt, lnk);
    CHECK(hv_clone_dir_ok(bad6, disk, sizeof disk, aux, sizeof aux, why, sizeof why) != 0, "an AUX that is a hard link to the original is refused: %s", why);
    char viadir[PATH_MAX]; snprintf(viadir, sizeof viadir, "%s/clone-008", clones); symlink(bad6, viadir);
    CHECK(hv_clone_dir_ok(viadir, disk, sizeof disk, aux, sizeof aux, why, sizeof why) != 0, "the clone folder itself as a symbolic link is refused: %s", why);

    wr(root, "vm_ecid.txt", "ecid=0x12345678901\n", 0);
    CHECK(hv_clone_read_ecid(root) == 0x12345678901ull, "a valid ECID file is read");
    wr(root, "vm_ecid.txt", "ecid=0x1\n", 0); CHECK(hv_clone_read_ecid(root) == 0, "a value below 2^40 is refused");
    wr(root, "vm_ecid.txt", "ecid=0xffffffffffffffff\n", 0); CHECK(hv_clone_read_ecid(root) == 0, "a value above 2^62 is refused");
    wr(root, "vm_ecid.txt", "ecid=0x12zz\n", 0); CHECK(hv_clone_read_ecid(root) == 0, "garbage is refused");
    CHECK(hv_clone_read_ecid("/tmp/no-such-dir-clonedir") == 0, "a missing file is 0");
    char cmd[PATH_MAX + 20]; snprintf(cmd, sizeof cmd, "rm -rf %s", root); if (system(cmd)) {}
    printf(g_fail ? "CLONEDIR FAILED (%d)\n" : "CLONEDIR OK\n", g_fail);
    return g_fail != 0;
}
