#define _GNU_SOURCE
#include "disk_policy.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static void path_of(const char *dir, char *p, size_t cap) { snprintf(p, cap, "%s/%s", dir, HV_INSTALL_DISK_NAME); }
#define NOTE(...) do { if (note && cap > 0) snprintf(note, (size_t)cap, __VA_ARGS__); } while (0)

int hv_install_disk_name_removable_by_recreate(const char *name) { return !strncmp(name, "blank_", 6); }

HvDiskState hv_install_disk_check(const char *dir, HvDiskInfo *info, char *note, int cap) {
    HvDiskInfo local; if (!info) info = &local;
    memset(info, 0, sizeof *info);
    struct statvfs sv;
    if (statvfs(dir, &sv) == 0) info->dir_free = (uint64_t)sv.f_bavail * sv.f_frsize;
    char p[1100]; path_of(dir, p, sizeof p);
    struct stat st;
    if (lstat(p, &st) != 0) {
        if (errno == ENOENT) { info->state = HV_DISK_MISSING; NOTE("%s does not exist; the design value is %llu bytes (128 GiB, sparse); %llu bytes free on that file system. Nothing was created.", HV_INSTALL_DISK_NAME, (unsigned long long)HV_INSTALL_DISK_BYTES, (unsigned long long)info->dir_free); return info->state; }
        info->state = HV_DISK_ERROR; NOTE("cannot look at %s: %s", HV_INSTALL_DISK_NAME, strerror(errno)); return info->state;
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
    char p[1100]; path_of(dir, p, sizeof p);
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) { NOTE(errno == EEXIST ? "%s already exists: nothing was created or changed" : "could not create %s: %s", HV_INSTALL_DISK_NAME, strerror(errno)); return -1; }
    if (ftruncate(fd, (off_t)HV_INSTALL_DISK_BYTES) != 0) {
        int e = errno; close(fd); unlink(p);   /* the file is ours (just created, O_EXCL): removing it undoes only this call */
        NOTE("could not size %s to %llu bytes: %s; the new file was removed", HV_INSTALL_DISK_NAME, (unsigned long long)HV_INSTALL_DISK_BYTES, strerror(e)); return -1;
    }
    close(fd);
    NOTE("created %s as a sparse file of %llu bytes", HV_INSTALL_DISK_NAME, (unsigned long long)HV_INSTALL_DISK_BYTES);
    return 0;
}

int hv_install_disk_reset(const char *dir, const char *confirm, char *note, int cap) {
    if (!confirm || strcmp(confirm, HV_INSTALL_DISK_RESET_CONFIRM)) { NOTE("not reset: the confirmation text does not match; nothing was changed"); return -1; }
    char p[1100]; path_of(dir, p, sizeof p);
    struct stat st;
    if (lstat(p, &st) != 0) { NOTE("nothing to reset: %s does not exist", HV_INSTALL_DISK_NAME); return -1; }
    if (!S_ISREG(st.st_mode)) { NOTE("not reset: %s is not a regular file", HV_INSTALL_DISK_NAME); return -1; }
    if (unlink(p) != 0) { NOTE("could not remove %s: %s", HV_INSTALL_DISK_NAME, strerror(errno)); return -1; }
    NOTE("%s was removed on explicit confirmation; no new disk was created", HV_INSTALL_DISK_NAME);
    return 0;
}
