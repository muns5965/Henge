#ifndef HENGEVM_DISK_POLICY_H
#define HENGEVM_DISK_POLICY_H

#include <stdint.h>

/* The install disk of the virtual Mac: one sparse file, kept for good.
     name      macos_disk.img   (never starts with "blank_", which is the only prefix vmrun --recreate-blank may delete)
     size      128 GiB = 137,438,953,472 bytes, a sparse file (it takes only what the guest writes)
   Rules: an existing file is never replaced, truncated, extended or shrunk by this module; a size that differs from the design value is REPORTED and needs an explicit
   decision by the user; there is no automatic re-creation. Resetting needs the exact confirmation text below.
   The directory is opened once and every step is relative to it (no link followed, device/inode of the file re-checked right before the removal); create and reset refuse a
   directory that is not owned by the caller or is writable by group/others. A swap of the file by someone who can write to that directory in the last instant before
   unlinkat cannot be excluded by any check, so the directory must be the app's private one.
   Nothing in the app calls the creating or resetting functions yet: creating the install disk is a separate approval. The tests run them in a temporary directory. */
#define HV_INSTALL_DISK_NAME "macos_disk.img"
#define HV_INSTALL_DISK_BYTES (128ULL << 30)
#define HV_INSTALL_DISK_RESET_CONFIRM "ERASE macos_disk.img"

typedef enum { HV_DISK_MISSING = 0, HV_DISK_OK, HV_DISK_SIZE_MISMATCH, HV_DISK_NOT_A_FILE, HV_DISK_ERROR } HvDiskState;
typedef struct { HvDiskState state; uint64_t size, allocated, dir_free; } HvDiskInfo;

/* Read-only. dir is the directory that holds (or will hold) the disk. */
HvDiskState hv_install_disk_check(const char *dir, HvDiskInfo *info, char *note, int cap);
/* Creates the sparse file ONLY when it does not exist (O_EXCL); returns 0 on success, -1 otherwise (note says why). Never touches an existing file. */
int hv_install_disk_create(const char *dir, char *note, int cap);
/* Deletes the disk ONLY when confirm equals HV_INSTALL_DISK_RESET_CONFIRM exactly; returns 0, or -1 (note says why). Does not create a new one. */
int hv_install_disk_reset(const char *dir, const char *confirm, char *note, int cap);
/* The rule vmrun --recreate-blank uses: 1 if a file of this name may be removed by it. */
int hv_install_disk_name_removable_by_recreate(const char *name);

#endif
