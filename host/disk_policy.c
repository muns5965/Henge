#define _GNU_SOURCE
#include "disk_policy.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <sys/types.h>

/* The directory is opened ONCE and every later step is relative to that descriptor (fstatat/openat/unlinkat with the plain name, symbolic links not followed), so
   a path component that is swapped for a link afterwards cannot redirect the step. What remains is a swap of the FILE between the last identity check and unlinkat:
   that needs write access to this directory, which is why create/reset refuse a directory that is not ours or is writable by others. */
static int open_dir(const char *dir) { return open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC); }
static int dir_trusted(int dfd) {
    struct stat st;
    return fstat(dfd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & (S_IWGRP | S_IWOTH));
}
#define NOTE(...) do { if (note && cap > 0) snprintf(note, (size_t)cap, __VA_ARGS__); } while (0)

int hv_install_disk_name_removable_by_recreate(const char *name) { return !strncmp(name, "blank_", 6); }

HvDiskState hv_install_disk_check(const char *dir, HvDiskInfo *info, char *note, int cap) {
    HvDiskInfo local; if (!info) info = &local;
    memset(info, 0, sizeof *info);
    struct statvfs sv;
    if (statvfs(dir, &sv) == 0) info->dir_free = (uint64_t)sv.f_bavail * sv.f_frsize;
    int dfd = open_dir(dir);
    if (dfd < 0) { info->state = HV_DISK_ERROR; NOTE("cannot open the directory: %s", strerror(errno)); return info->state; }
    struct stat st;
    int r = fstatat(dfd, HV_INSTALL_DISK_NAME, &st, AT_SYMLINK_NOFOLLOW), e = errno;
    close(dfd);
    if (r != 0) {
        if (e == ENOENT) { info->state = HV_DISK_MISSING; NOTE("%s does not exist; the design value is %llu bytes (128 GiB, sparse); %llu bytes free on that file system. Nothing was created.", HV_INSTALL_DISK_NAME, (unsigned long long)HV_INSTALL_DISK_BYTES, (unsigned long long)info->dir_free); return info->state; }
        info->state = HV_DISK_ERROR; NOTE("cannot look at %s: %s", HV_INSTALL_DISK_NAME, strerror(e)); return info->state;
    }
    if (!S_ISREG(st.st_mode)) { info->state = HV_DISK_NOT_A_FILE; NOTE("%s exists but is not a regular file; it was left alone", HV_INSTALL_DISK_NAME); return info->state; }
    info->size = (uint64_t)st.st_size;
    info->allocated = (uint64_t)st.st_blocks * 512;
    if (info->size != HV_INSTALL_DISK_BYTES) {
        info->state = HV_DISK_SIZE_MISMATCH;
        NOTE("%s is %llu bytes, the design value is %llu: NOT resized (an explicit decision is needed); %llu bytes of it are allocated", HV_INSTALL_DISK_NAME, (unsigned long long)info->size, (unsigned long long)HV_INSTALL_DISK_BYTES, (unsigned long long)info->allocated);
        return info->state;
    }
    info->state = HV_DISK_OK;
    NOTE("%s is %llu bytes (128 GiB) with %llu bytes allocated; kept as it is; %llu bytes free on the file system (a sparse file grows as the guest writes: free space is not a guarantee)", HV_INSTALL_DISK_NAME, (unsigned long long)info->size, (unsigned long long)info->allocated, (unsigned long long)info->dir_free);
    return info->state;
}

int hv_install_disk_create(const char *dir, char *note, int cap) {
    int dfd = open_dir(dir);
    if (dfd < 0) { NOTE("could not open the directory: %s", strerror(errno)); return -1; }
    if (!dir_trusted(dfd)) { close(dfd); NOTE("refused: the directory is not owned by this user or is writable by others"); return -1; }
    int fd = openat(dfd, HV_INSTALL_DISK_NAME, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { int e = errno; close(dfd); NOTE(e == EEXIST ? "%s already exists: nothing was created or changed" : "could not create %s: %s", HV_INSTALL_DISK_NAME, strerror(e)); return -1; }
    if (ftruncate(fd, (off_t)HV_INSTALL_DISK_BYTES) != 0) {
        int e = errno;
        /* remove it only if the name still leads to the file we created (same device and inode) */
        struct stat a, b;
        if (fstat(fd, &a) == 0 && fstatat(dfd, HV_INSTALL_DISK_NAME, &b, AT_SYMLINK_NOFOLLOW) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino) unlinkat(dfd, HV_INSTALL_DISK_NAME, 0);
        close(fd); close(dfd);
        NOTE("could not size %s to %llu bytes: %s; the new file was removed", HV_INSTALL_DISK_NAME, (unsigned long long)HV_INSTALL_DISK_BYTES, strerror(e)); return -1;
    }
    close(fd); close(dfd);
    NOTE("created %s as a sparse file of %llu bytes", HV_INSTALL_DISK_NAME, (unsigned long long)HV_INSTALL_DISK_BYTES);
    return 0;
}

int hv_install_disk_reset(const char *dir, const char *confirm, char *note, int cap) {
    if (!confirm || strcmp(confirm, HV_INSTALL_DISK_RESET_CONFIRM)) { NOTE("not reset: the confirmation text does not match; nothing was changed"); return -1; }
    int dfd = open_dir(dir);
    if (dfd < 0) { NOTE("not reset: could not open the directory: %s", strerror(errno)); return -1; }
    if (!dir_trusted(dfd)) { close(dfd); NOTE("not reset: the directory is not owned by this user or is writable by others"); return -1; }
    /* open the file itself (no link followed): what is removed must be the regular file that was looked at */
    int fd = openat(dfd, HV_INSTALL_DISK_NAME, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { int e = errno; close(dfd); NOTE(e == ENOENT ? "nothing to reset: %s does not exist" : "not reset: %s cannot be opened as a plain file (%s)", HV_INSTALL_DISK_NAME, strerror(e)); return -1; }
    struct stat a, b;
    if (fstat(fd, &a) != 0 || !S_ISREG(a.st_mode)) { close(fd); close(dfd); NOTE("not reset: %s is not a regular file", HV_INSTALL_DISK_NAME); return -1; }
    if (fstatat(dfd, HV_INSTALL_DISK_NAME, &b, AT_SYMLINK_NOFOLLOW) != 0 || a.st_dev != b.st_dev || a.st_ino != b.st_ino) { close(fd); close(dfd); NOTE("not reset: %s changed while it was being checked", HV_INSTALL_DISK_NAME); return -1; }
    int r = unlinkat(dfd, HV_INSTALL_DISK_NAME, 0), e = errno;
    close(fd); close(dfd);
    if (r != 0) { NOTE("could not remove %s: %s", HV_INSTALL_DISK_NAME, strerror(e)); return -1; }
    NOTE("%s was removed on explicit confirmation; no new disk was created", HV_INSTALL_DISK_NAME);
    return 0;
}
