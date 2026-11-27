#ifndef HENGEVM_DISK_H
#define HENGEVM_DISK_H

#include <stdint.h>

/* A raw disk image file, optionally starting `offset` bytes in (QEMU's vmapple
   notes say the first 16 KB of auxiliary.bin is metadata the guest never sees). */
typedef struct HvDisk {
    int fd;
    int wfd;         /* writes, past the host's cache (F_NOCACHE); -1: through fd */
    uint64_t offset;
    uint64_t size;   /* bytes visible to the guest */
    int readonly;
    uint64_t write_calls, write_refused;   /* hv_disk_write calls, and those turned away (read-only or out of range); read with hv_disk_write_stats */
    uint64_t write_failed, write_bytes;    /* writes that passed the checks but whose pwrite failed; bytes pwrite reported written (not synced) */
    uint64_t flush_calls, flush_failed;    /* hv_disk_flush calls, and those whose sync failed */
    int flush_last;                        /* HV_FLUSH_* result of the newest hv_disk_flush call */
} HvDisk;
#define HV_FLUSH_NEVER (-1)     /* hv_disk_flush was never called */
#define HV_FLUSH_OK 0           /* a sync was done and succeeded */
#define HV_FLUSH_FAILED 1       /* the sync failed */
#define HV_FLUSH_SKIPPED 2      /* nothing was synced: read-only disk, or HV_FLUSH knob 1 */

int hv_disk_open(HvDisk *d, const char *path, uint64_t offset, int readonly);
void hv_disk_close(HvDisk *d);
/* Return 0, or -1 on an I/O error or an access past the end. */
int hv_disk_read(HvDisk *d, uint64_t off, void *buf, uint64_t len);
int hv_disk_write(HvDisk *d, uint64_t off, const void *buf, uint64_t len);
int hv_disk_flush(HvDisk *d);
/* How many writes the guest's devices asked for since the disk was opened (any thread). */
void hv_disk_write_stats(const HvDisk *d, uint64_t *calls, uint64_t *refused);
/* Counters of one disk, for the runner's log. COUNTS and file metadata only: the file's contents are never read or hashed here. A write that the host accepted (pwrite) and a flush that returned success
   say what the HOST did with the file; they do not say that the guest's boot device kept anything. */
typedef struct HvDiskStats {
    uint64_t write_calls, write_refused, write_failed, write_bytes;
    uint64_t flush_calls, flush_failed;
    int flush_last;               /* HV_FLUSH_* */
    uint64_t file_size, mtime_s;  /* fstat of the backing file (whole file, including any hidden offset); 0 when unknown */
} HvDiskStats;
void hv_disk_stats(const HvDisk *d, HvDiskStats *s);

#endif
