#ifndef HENGEVM_G7_FRAME_FILE_H
#define HENGEVM_G7_FRAME_FILE_H
/* integ7b T9: the full-resolution picture for the app, as a FILE the runner writes on request (the "frame" command), next to the shared file in the app's files folder.

   Why a file and not the shared file itself: host/frame_shm.h's contract has ONE synchronisation, a pid word that is taken with compare-and-swap, and says that nobody reads
   pixels without it (a sequence number around a plain copy would still be a data race). The app's Java code cannot take that lock (no atomics on a mapped file there that this build can rely
   on) and a JNI wrapper cannot be built or tried in this environment. So the runner, which already holds the lock correctly (hv_shm_read_frame), copies the newest picture out and
   publishes it here, whole, by temp file + rename: a reader that opened the old file keeps the old inode, a reader that opens later sees the new one; a file is never modified in place.
   The header and the trailer both carry the picture's sequence number, so a reader can still tell a cut-off or mixed file (a crash while writing the temp file never reaches the final name).

   Layout, all integers little endian:
     0   char[8]  "G7FRAME1"
     8   u32 width      12  u32 height      16  u32 format (1 = BGRA 8888, byte order B G R A, as in the shared file)
     20  u32 stride in bytes (= width * 4, tightly packed)     24  u64 sequence number (the shared file's frame_seq of this picture)
     32  pixels, stride * height bytes
     32 + stride * height   char[8] "G7FREND1", u64 sequence number (the same)
   The picture is whatever the runner publishes into the shared file, at ITS size (no resampling): the PVG surface (1920x1080 so far) or, before the PVG surface exists, the early
   framebuffer from the boot args, which can have another size. The FRAME line says which size it was (src=WxH). */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define G7FF_NAME "g7_frame.bin"
#define G7FF_TMP "g7_frame.tmp"
#define G7FF_HDR 32u
#define G7FF_TRL 16u

static inline void g7ff_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline void g7ff_put64(uint8_t *p, uint64_t v) { g7ff_put32(p, (uint32_t)v); g7ff_put32(p + 4, (uint32_t)(v >> 32)); }

static inline int g7ff_write_all(int fd, const uint8_t *p, size_t n) {
    while (n) { ssize_t k = write(fd, p, n); if (k < 0) { if (errno == EINTR) continue; return -1; } if (k == 0) return -1; p += k; n -= (size_t)k; }
    return 0;
}

/* Writes dir/g7_frame.tmp and renames it to dir/g7_frame.bin. bgra is tightly packed (width * 4 per row). 0 on success, else -1 (errno set; the old final file is untouched). */
static inline int g7ff_publish(const char *dir, const uint8_t *bgra, uint32_t w, uint32_t h, uint64_t seq) {
    char tmp[1024], fin[1024];
    if (!dir || !bgra || !w || !h || w > 16384 || h > 16384) { errno = EINVAL; return -1; }
    if (snprintf(tmp, sizeof tmp, "%s/%s", dir, G7FF_TMP) >= (int)sizeof tmp || snprintf(fin, sizeof fin, "%s/%s", dir, G7FF_NAME) >= (int)sizeof fin) { errno = ENAMETOOLONG; return -1; }
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    uint8_t hd[G7FF_HDR], tr[G7FF_TRL];
    memset(hd, 0, sizeof hd); memcpy(hd, "G7FRAME1", 8);
    g7ff_put32(hd + 8, w); g7ff_put32(hd + 12, h); g7ff_put32(hd + 16, 1); g7ff_put32(hd + 20, w * 4u); g7ff_put64(hd + 24, seq);
    memcpy(tr, "G7FREND1", 8); g7ff_put64(tr + 8, seq);
    int bad = g7ff_write_all(fd, hd, sizeof hd) || g7ff_write_all(fd, bgra, (size_t)w * 4u * h) || g7ff_write_all(fd, tr, sizeof tr);
    int e = errno;
    if (close(fd) != 0 && !bad) { bad = 1; e = errno; }
    if (bad) { unlink(tmp); errno = e; return -1; }
    if (rename(tmp, fin) != 0) { e = errno; unlink(tmp); errno = e; return -1; }
    return 0;
}

#endif
