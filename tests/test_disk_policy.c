/* host/disk_policy.c in a temporary directory: the install disk is created only when missing, kept when it exists, never resized, reset only on the exact text. Linux. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../host/disk_policy.h"

static int g_fail;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

int main(void) {
    char dir[] = "/tmp/hvdisk-XXXXXX";
    if (!mkdtemp(dir)) return 2;
    char note[400], p[200], marker[16] = "keep-this-data";
    snprintf(p, sizeof p, "%s/%s", dir, HV_INSTALL_DISK_NAME);
    HvDiskInfo i;
    CHECK(HV_INSTALL_DISK_BYTES == 137438953472ULL, "the design size is 137,438,953,472 bytes");
    CHECK(!hv_install_disk_name_removable_by_recreate(HV_INSTALL_DISK_NAME), "macos_disk.img is not removable by --recreate-blank");
    CHECK(hv_install_disk_name_removable_by_recreate("blank_disk.img"), "blank_disk.img is (the existing rule)");

    CHECK(hv_install_disk_check(dir, &i, note, sizeof note) == HV_DISK_MISSING, "missing is reported, not created (%s)", note);
    CHECK(access(p, F_OK) != 0, "the check created nothing");
    CHECK(hv_install_disk_create(dir, note, sizeof note) == 0, "create: %s", note);
    struct stat st; stat(p, &st);
    CHECK((uint64_t)st.st_size == HV_INSTALL_DISK_BYTES, "size is exactly 128 GiB");
    CHECK((uint64_t)st.st_blocks * 512 < (64ULL << 20), "sparse: %llu bytes allocated of 128 GiB", (unsigned long long)st.st_blocks * 512);
    CHECK(hv_install_disk_check(dir, &i, note, sizeof note) == HV_DISK_OK, "check: OK (%s)", note);

    int fd = open(p, O_WRONLY); pwrite(fd, marker, sizeof marker, 4096); close(fd);
    CHECK(hv_install_disk_create(dir, note, sizeof note) == -1, "create again: refused (%s)", note);
    char rb[16] = {0}; fd = open(p, O_RDONLY); pread(fd, rb, sizeof rb, 4096); close(fd);
    CHECK(!memcmp(rb, marker, sizeof marker), "the existing data is still there");

    truncate(p, 64ULL << 30);
    CHECK(hv_install_disk_check(dir, &i, note, sizeof note) == HV_DISK_SIZE_MISMATCH, "a 64 GiB file is reported as a mismatch (%s)", note);
    stat(p, &st);
    CHECK((uint64_t)st.st_size == (64ULL << 30), "and it was NOT resized");
    CHECK(hv_install_disk_create(dir, note, sizeof note) == -1, "create over a mismatching file: refused");

    CHECK(hv_install_disk_reset(dir, NULL, note, sizeof note) == -1 && access(p, F_OK) == 0, "reset without confirmation: refused");
    CHECK(hv_install_disk_reset(dir, "erase macos_disk.img", note, sizeof note) == -1 && access(p, F_OK) == 0, "reset with the wrong text: refused");
    CHECK(hv_install_disk_reset(dir, HV_INSTALL_DISK_RESET_CONFIRM, note, sizeof note) == 0 && access(p, F_OK) != 0, "reset with the exact text: removed (%s)", note);
    CHECK(hv_install_disk_check(dir, &i, note, sizeof note) == HV_DISK_MISSING, "and nothing was created in its place");

    char blank[220]; snprintf(blank, sizeof blank, "%s/blank_disk.img", dir);
    int bf = open(blank, O_WRONLY | O_CREAT, 0600); close(bf);
    CHECK(hv_install_disk_create(dir, note, sizeof note) == 0 && access(blank, F_OK) == 0, "a blank_ file next to it is not affected");
    unlink(blank); unlink(p);

    /* names that are not the plain file: a link, a FIFO, a directory. Nothing behind them is touched. */
    char victim[220]; snprintf(victim, sizeof victim, "%s/victim.dat", dir);
    int vf = open(victim, O_WRONLY | O_CREAT, 0600); write(vf, "precious", 8); close(vf);
    symlink(victim, p);
    CHECK(hv_install_disk_check(dir, &i, note, sizeof note) == HV_DISK_NOT_A_FILE, "a symbolic link under the name is reported as not a file (%s)", note);
    CHECK(hv_install_disk_reset(dir, HV_INSTALL_DISK_RESET_CONFIRM, note, sizeof note) == -1 && access(victim, F_OK) == 0, "reset on a symbolic link: refused, the link target is untouched (%s)", note);
    CHECK(hv_install_disk_create(dir, note, sizeof note) == -1 && stat(victim, &st) == 0 && st.st_size == 8, "create over a symbolic link: refused, the target is untouched");
    unlink(p);
    mkfifo(p, 0600);
    CHECK(hv_install_disk_reset(dir, HV_INSTALL_DISK_RESET_CONFIRM, note, sizeof note) == -1 && access(p, F_OK) == 0, "reset on a FIFO: refused (%s)", note);
    unlink(p);
    mkdir(p, 0700);
    CHECK(hv_install_disk_reset(dir, HV_INSTALL_DISK_RESET_CONFIRM, note, sizeof note) == -1 && access(p, F_OK) == 0, "reset on a directory of that name: refused (%s)", note);
    rmdir(p);
    unlink(victim);

    /* a directory that others can write to is not trusted for create / reset */
    chmod(dir, 0777);
    CHECK(hv_install_disk_create(dir, note, sizeof note) == -1 && access(p, F_OK) != 0, "create in a world-writable directory: refused (%s)", note);
    chmod(dir, 0700);
    CHECK(hv_install_disk_create(dir, note, sizeof note) == 0, "create works again once the directory is private");
    chmod(dir, 0770);
    CHECK(hv_install_disk_reset(dir, HV_INSTALL_DISK_RESET_CONFIRM, note, sizeof note) == -1 && access(p, F_OK) == 0, "reset in a group-writable directory: refused (%s)", note);
    chmod(dir, 0700);
    CHECK(hv_install_disk_check("/nonexistent-dir-xyz", &i, note, sizeof note) == HV_DISK_ERROR, "a directory that cannot be opened: reported as an error (%s)", note);
    unlink(p); rmdir(dir);
    printf("%s (%d failed)\n", g_fail ? "DISK POLICY FAILED" : "DISK POLICY OK", g_fail);
    return g_fail ? 1 : 0;
}
