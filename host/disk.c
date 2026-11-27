#include <fcntl.h>
#include <errno.h>
#include "../core/machine.h"
#include "disk.h"
#include "../core/waitdiag.h"

/* F_NOCACHE and F_BARRIERFSYNC are Apple's. Elsewhere: no uncached writes (the
   caller treats that as "not available" and keeps writing through the cache),
   and the barrier flush is a plain fsync. */
#ifdef F_NOCACHE
#define HV_FD_NOCACHE(fd) fcntl((fd), F_NOCACHE, 1)
#else
#define HV_FD_NOCACHE(fd) (errno = ENOTSUP, -1)
#endif
#ifdef F_BARRIERFSYNC
#define HV_FD_BARRIER_FSYNC(fd) fcntl((fd), F_BARRIERFSYNC)
#else
#define HV_FD_BARRIER_FSYNC(fd) fsync(fd)
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>

#define DISK_IO_CHUNK (128u << 20)

int hv_disk_open(HvDisk *d, const char *path, uint64_t offset, int readonly) {
    d->wfd = -1;
    d->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (d->fd < 0) return -1;
    struct stat st;
    if (fstat(d->fd, &st) != 0 || (uint64_t)st.st_size < offset) {
        close(d->fd);
        d->fd = -1;   /* as the original: a failed open leaves no descriptor number behind that a later close could hit */
        return -1;
    }
    d->offset = offset;
    d->size = (uint64_t)st.st_size - offset;
    d->readonly = readonly;
    d->write_calls = d->write_refused = d->write_failed = d->write_bytes = 0;
    d->flush_calls = d->flush_failed = 0;
    d->flush_last = HV_FLUSH_NEVER;
    /* HV_WRITE_CACHE=0: writes skip the host's cache (F_NOCACHE), so a long run of them cannot pile up
       gigabytes of dirty pages; reads stay cached, and the kernel keeps the two coherent. Off by default:
       on the Mac bench big writes took 2-3x as long that way (2 GB + sync 4-6 s -> 9-17 s) and everyday
       work was no faster. The phone's installer turns it on for the restore, which writes ~15 GB at once. */
    const char *wc = getenv("HV_WRITE_CACHE");
    if (!readonly && wc && !atoi(wc)) {
        d->wfd = open(path, O_RDWR);
        if (d->wfd >= 0 && HV_FD_NOCACHE(d->wfd) == -1) {
            close(d->wfd);
            d->wfd = -1;
        }
    }
    return 0;
}

void hv_disk_close(HvDisk *d) {
    if (d->fd >= 0) close(d->fd);
    if (d->wfd >= 0) close(d->wfd);
    d->fd = d->wfd = -1;
}

static int disk_read_impl(HvDisk *d, uint64_t off, void *buf, uint64_t len) {
    if (off > d->size || len > d->size - off) return -1;
    uint8_t *p = buf;
    while (len) {
        size_t count = len > DISK_IO_CHUNK ? DISK_IO_CHUNK : (size_t)len;
        ssize_t n = pread(d->fd, p, count, (off_t)(d->offset + off));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

static int disk_write_impl(HvDisk *d, uint64_t off, const void *buf, uint64_t len) {
    __atomic_add_fetch(&d->write_calls, 1, __ATOMIC_RELAXED);
    if (d->readonly || off > d->size || len > d->size - off) {
        __atomic_add_fetch(&d->write_refused, 1, __ATOMIC_RELAXED);
        return -1;
    }
    const uint8_t *p = buf;
    while (len) {
        size_t count = len > DISK_IO_CHUNK ? DISK_IO_CHUNK : (size_t)len;
        ssize_t n = pwrite(d->wfd >= 0 ? d->wfd : d->fd, p, count, (off_t)(d->offset + off));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { __atomic_add_fetch(&d->write_failed, 1, __ATOMIC_RELAXED); return -1; }
        __atomic_add_fetch(&d->write_bytes, (uint64_t)n, __ATOMIC_RELAXED);
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

/* Diagnostic timing only (core/waitdiag.h): the pread / pwrite loops, bracketed around the unchanged bodies above. */
int hv_disk_read(HvDisk *d, uint64_t off, void *buf, uint64_t len) {
    uint64_t wd0 = hv_wd_begin(HV_WD_DISK_READ);
    int rc = disk_read_impl(d, off, buf, len);
    hv_wd_end(HV_WD_DISK_READ, wd0);
    return rc;
}

int hv_disk_write(HvDisk *d, uint64_t off, const void *buf, uint64_t len) {
    uint64_t wd0 = hv_wd_begin(HV_WD_DISK_WRITE);
    int rc = disk_write_impl(d, off, buf, len);
    hv_wd_end(HV_WD_DISK_WRITE, wd0);
    return rc;
}

/* HV_FLUSH: 0 fsync; 1 nothing here, what was written is safe in the host's cache as long as the
   host runs; 2 fcntl(F_BARRIERFSYNC). */
static int disk_flush_impl(HvDisk *d, int *state) {
    int rc;
    uint64_t wd0;
    *state = HV_FLUSH_OK;
    if (d->readonly) { *state = HV_FLUSH_SKIPPED; return 0; }
    switch (hv_knob[HV_K_FLUSH]) {
    case 1: *state = HV_FLUSH_SKIPPED; return 0;
    case 2: wd0 = hv_wd_begin(HV_WD_DISK_FSYNC); rc = HV_FD_BARRIER_FSYNC(d->fd) == -1 ? -1 : 0; hv_wd_end(HV_WD_DISK_FSYNC, wd0); break;
    default: wd0 = hv_wd_begin(HV_WD_DISK_FSYNC); rc = fsync(d->fd); hv_wd_end(HV_WD_DISK_FSYNC, wd0); break;
    }
    if (rc != 0) *state = HV_FLUSH_FAILED;
    return rc;
}
int hv_disk_flush(HvDisk *d) {
    int state, rc = disk_flush_impl(d, &state);
    __atomic_add_fetch(&d->flush_calls, 1, __ATOMIC_RELAXED);
    if (state == HV_FLUSH_FAILED) __atomic_add_fetch(&d->flush_failed, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&d->flush_last, state, __ATOMIC_RELAXED);
    return rc;
}

void hv_disk_write_stats(const HvDisk *d, uint64_t *calls, uint64_t *refused) {
    *calls = __atomic_load_n(&d->write_calls, __ATOMIC_RELAXED);
    *refused = __atomic_load_n(&d->write_refused, __ATOMIC_RELAXED);
}

void hv_disk_stats(const HvDisk *d, HvDiskStats *s) {
    s->write_calls = __atomic_load_n(&d->write_calls, __ATOMIC_RELAXED);
    s->write_refused = __atomic_load_n(&d->write_refused, __ATOMIC_RELAXED);
    s->write_failed = __atomic_load_n(&d->write_failed, __ATOMIC_RELAXED);
    s->write_bytes = __atomic_load_n(&d->write_bytes, __ATOMIC_RELAXED);
    s->flush_calls = __atomic_load_n(&d->flush_calls, __ATOMIC_RELAXED);
    s->flush_failed = __atomic_load_n(&d->flush_failed, __ATOMIC_RELAXED);
    s->flush_last = __atomic_load_n(&d->flush_last, __ATOMIC_RELAXED);
    s->file_size = s->mtime_s = 0;
    struct stat st;
    if (d->fd >= 0 && fstat(d->fd, &st) == 0) { s->file_size = (uint64_t)st.st_size; s->mtime_s = (uint64_t)st.st_mtime; }
}
