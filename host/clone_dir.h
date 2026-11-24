#ifndef HENGEVM_CLONE_DIR_H
#define HENGEVM_CLONE_DIR_H
/* The two checks of host/ui_runner_vm_clone.c that decide whether anything may be written: whether a folder is a finished, plain clone, and the ECID read. Header-only so that tests/test_clone_dir.c
   runs the same code on a Linux machine. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Whether the clone directory is what it must be. Returns 0 and fills disk/aux, or -1 with a reason. */
static inline int hv_clone_dir_ok(const char *dir, char *disk, size_t dcap, char *aux, size_t acap, char *why, size_t wcap) {
#define BAD(...) do { snprintf(why, wcap, __VA_ARGS__); return -1; } while (0)
    char real[PATH_MAX];
    if (!dir || dir[0] != '/') BAD("--clone-dir must be an absolute path");
    if (!realpath(dir, real) || strcmp(real, dir)) BAD("--clone-dir is not a plain path (a link or a relative part)");
    const char *last = strrchr(real, '/'); if (!last || strlen(last) != 10 || strncmp(last, "/clone-", 7) || !isdigit((unsigned char)last[7]) || !isdigit((unsigned char)last[8]) || !isdigit((unsigned char)last[9])) BAD("the folder is not named clone-NNN");
    if (last - real < 7 || strncmp(last - 7, "/clones", 7)) BAD("the folder is not inside a clones folder");
    int dfd = open(real, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) BAD("cannot open the clone folder: %s", strerror(errno));
    struct stat st_disk, st_aux, st;
    if (fstatat(dfd, "macos_disk.img", &st_disk, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st_disk.st_mode) || st_disk.st_nlink != 1) { close(dfd); BAD("macos_disk.img is missing, not a plain file or has another link"); }
    if (fstatat(dfd, "macos_aux.img", &st_aux, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st_aux.st_mode) || st_aux.st_nlink != 1) { close(dfd); BAD("macos_aux.img is missing, not a plain file or has another link"); }
    if (fstatat(dfd, "g7_state.txt", &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)) { close(dfd); BAD("g7_state.txt is missing or not a plain file"); }
    char mk[600]; int mfd = openat(dfd, "clone_state.txt", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (mfd < 0) { close(dfd); BAD("the clone has no completion marker (clone_state.txt): it is not a finished clone"); }
    ssize_t n = read(mfd, mk, sizeof mk - 1); close(mfd);
    if (n <= 0) { close(dfd); BAD("the completion marker is empty"); }
    mk[n] = 0;
    if (strncmp(mk, "state=clone_complete\n", 21)) { close(dfd); BAD("the marker does not say clone_complete"); }
    const char *p = strstr(mk, "logical_disk_bytes=");
    if (!p || strtoull(p + 19, NULL, 10) != (unsigned long long)st_disk.st_size) { close(dfd); BAD("the marker's disk size does not match macos_disk.img"); }
    char sf[160]; int sfd = openat(dfd, "g7_state.txt", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    ssize_t sn = sfd >= 0 ? read(sfd, sf, sizeof sf - 1) : -1; if (sfd >= 0) close(sfd);
    if (sn <= 0) { close(dfd); BAD("g7_state.txt cannot be read"); }
    sf[sn] = 0;
    if (!strstr(sf, "\necid_tag=")) { close(dfd); BAD("the clone is not tied to an ECID (no ecid_tag line)"); }
    int dfd2 = dup(dfd); DIR *d = dfd2 >= 0 ? fdopendir(dfd2) : NULL;
    if (!d) { if (dfd2 >= 0) close(dfd2); close(dfd); BAD("cannot list the clone folder"); }
    struct dirent *de; int partial = 0;
    while ((de = readdir(d))) { size_t l = strlen(de->d_name); if (l >= 8 && !strcmp(de->d_name + l - 8, ".partial")) partial = 1; }
    closedir(d); close(dfd);
    if (partial) BAD("the clone folder still has .partial files");
    snprintf(disk, dcap, "%s/macos_disk.img", real); snprintf(aux, acap, "%s/macos_aux.img", real);
    return 0;
#undef BAD
}

/* "ecid=0x<hex>" in <dir>/vm_ecid.txt, 2^40 <= value < 2^62. 0 on any problem (the value is never printed). */
static inline uint64_t hv_clone_read_ecid(const char *dir) {
    char path[PATH_MAX], t[64];
    if (snprintf(path, sizeof path, "%s/vm_ecid.txt", dir) >= (int)sizeof path) return 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); if (fd < 0) return 0;
    ssize_t n = read(fd, t, sizeof t - 1); close(fd); if (n < 8) return 0; t[n] = 0;
    while (n > 0 && (t[n - 1] == '\n' || t[n - 1] == '\r' || t[n - 1] == ' ')) t[--n] = 0;
    if (strncmp(t, "ecid=0x", 7) || n > 23) return 0;
    char *end; unsigned long long v = strtoull(t + 7, &end, 16);
    if (*end || v < (1ull << 40) || v >= (1ull << 62)) return 0;
    return v;
}

#endif
