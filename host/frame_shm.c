#define _GNU_SOURCE
#include "frame_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SHM_MAGIC 0x31534d48u   /* "HMS1" */
#define SHM_VERSION 1u
#define HDR_BYTES 16384u

typedef struct {
    uint32_t magic, version;        /* magic is written last by the creator (release) and read first by the opener (acquire) */
    uint32_t lock;                  /* 0 = free, else the pid of the holder: guards everything below except the atomics (see lock() below) */
    /* atomics, read and written without the lock */
    uint32_t state, runner_pid, app_closed;
    uint64_t heartbeat;
    /* under the lock */
    uint32_t width, height;         /* of the picture held; 0 = none */
    uint64_t frame_seq;
    int32_t ptr_valid, ptr_x, ptr_y;
    uint64_t frames, rejected, cmds_pushed, cmds_applied, cmds_dropped;
    uint32_t recovered;
    uint32_t cmd_head, cmd_tail;    /* counted up for ever (unsigned wrap is fine); slot = index % HV_SHM_CMD_SLOTS */
    HvShmCmd cmds[HV_SHM_CMD_SLOTS];
} Hdr;

_Static_assert(sizeof(Hdr) <= HDR_BYTES, "the header must fit in its page");

struct HvShm {
    int fd;
    Hdr *h;
    uint8_t *px;
    size_t len;
};

static size_t total_bytes(void) { return (size_t)HDR_BYTES + HV_SHM_PIXEL_BYTES; }

static HvShm *map_file(int fd) {
    void *m = mmap(NULL, total_bytes(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) return NULL;
    HvShm *s = calloc(1, sizeof *s);
    if (!s) { munmap(m, total_bytes()); return NULL; }
    s->fd = fd; s->h = m; s->px = (uint8_t *)m + HDR_BYTES; s->len = total_bytes();
    return s;
}

int hv_shm_create(const char *path, HvShm **out) {
    *out = NULL;
    unlink(path);   /* a fresh file: a mapping of an older one (a dead runner's) is not touched by what follows */
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)total_bytes()) != 0) { int e = errno; close(fd); unlink(path); errno = e; return -1; }
    HvShm *s = map_file(fd);
    if (!s) { int e = errno; close(fd); unlink(path); errno = e; return -1; }
    Hdr *h = s->h;
    memset(h, 0, sizeof *h);
    h->version = SHM_VERSION;
    __atomic_store_n(&h->state, (uint32_t)HV_SHM_STARTING, __ATOMIC_RELEASE);
    __atomic_store_n(&h->runner_pid, (uint32_t)getpid(), __ATOMIC_RELEASE);
    __atomic_store_n(&h->magic, SHM_MAGIC, __ATOMIC_RELEASE);
    *out = s;
    return 0;
}

int hv_shm_open(const char *path, HvShm **out) {
    *out = NULL;
    int fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size != total_bytes()) { close(fd); errno = EINVAL; return -1; }
    HvShm *s = map_file(fd);
    if (!s) { int e = errno; close(fd); errno = e; return -1; }
    if (__atomic_load_n(&s->h->magic, __ATOMIC_ACQUIRE) != SHM_MAGIC || s->h->version != SHM_VERSION) {
        munmap(s->h, s->len); close(fd); free(s); errno = EINVAL; return -1;
    }
    *out = s;
    return 0;
}

void hv_shm_close(HvShm *s) {
    if (!s) return;
    munmap(s->h, s->len);
    close(s->fd);
    free(s);
}

/* The lock is a word in the file: 0 = free, otherwise the holder's pid (acquire by compare-and-swap, release by a store). It is not a pthread mutex because Android's libc has no
   robust mutexes (pthread_mutexattr_setrobust does not exist there), and a lock that a killed process still "holds" would freeze the other side for good.
   Waiting is a short sleep loop (holders keep it for about a millisecond). If the holder's pid no longer exists (kill(pid, 0) says ESRCH), the waiter takes the lock over with one
   compare-and-swap (so only one waiter can), counts it, and drops the picture, which may be half written. A holder that is alive but slow is never taken over: the waiter gives up
   after HV_SHM_LOCK_MS and reports failure. (A holder that died but was not yet reaped by its parent still has a pid; its parent reaps it at once in both uses, so that is brief.)
   0 locked. -1 not locked (timeout). */
static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int lock(HvShm *s) {
    uint32_t me = (uint32_t)getpid();
    uint32_t *w = &s->h->lock;
    double deadline = 0;
    for (unsigned spin = 0;; spin++) {
        uint32_t owner = 0;
        if (__atomic_compare_exchange_n(w, &owner, me, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
        if (owner && owner != me && kill((pid_t)owner, 0) != 0 && errno == ESRCH) {
            if (__atomic_compare_exchange_n(w, &owner, me, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                s->h->recovered++;
                s->h->width = s->h->height = 0;   /* the picture may be half written */
                return 0;
            }
            continue;
        }
        if (!deadline) deadline = mono() + HV_SHM_LOCK_MS / 1000.0;
        else if (mono() >= deadline) return -1;
        if (spin < 20) sched_yield();
        else { struct timespec ts = {0, 100 * 1000}; nanosleep(&ts, NULL); }
    }
}
static void unlock(HvShm *s) { __atomic_store_n(&s->h->lock, 0u, __ATOMIC_RELEASE); }

int hv_shm_publish(HvShm *s, const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t stride) {
    if (lock(s) != 0) return -1;
    int r = 0;
    uint64_t need = (uint64_t)width * 4u;
    if (!bgra || !width || !height || stride < need || need * height > HV_SHM_PIXEL_BYTES) { s->h->rejected++; goto out; }
    for (uint32_t y = 0; y < height; y++) memcpy(s->px + (size_t)y * need, bgra + (size_t)y * stride, (size_t)need);
    s->h->width = width; s->h->height = height;
    s->h->frame_seq++; s->h->frames++;
    r = 1;
out:
    unlock(s);
    return r;
}

int hv_shm_read_frame(HvShm *s, uint64_t *seen, uint8_t *dst, size_t cap, uint32_t *width, uint32_t *height) {
    if (lock(s) != 0) return -1;
    int r;
    if (!s->h->width || !s->h->height) r = -1;
    else if (s->h->frame_seq == *seen) r = 0;
    else {
        size_t n = (size_t)s->h->width * s->h->height * 4u;
        if (!dst || n > cap) r = -1;
        else { memcpy(dst, s->px, n); *seen = s->h->frame_seq; *width = s->h->width; *height = s->h->height; r = 1; }
    }
    unlock(s);
    return r;
}

int hv_shm_push_cmd(HvShm *s, uint32_t type, int a, int b) {
    if (lock(s) != 0) return -1;
    int r;
    if (s->h->cmd_head - s->h->cmd_tail >= HV_SHM_CMD_SLOTS) { s->h->cmds_dropped++; r = 0; }
    else {
        HvShmCmd *c = &s->h->cmds[s->h->cmd_head % HV_SHM_CMD_SLOTS];
        c->type = type; c->a = a; c->b = b;
        s->h->cmd_head++;        /* the index moves last */
        s->h->cmds_pushed++;
        r = 1;
    }
    unlock(s);
    return r;
}

int hv_shm_pop_cmds(HvShm *s, HvShmCmd *out, int max) {
    if (lock(s) != 0) return -1;
    int n = 0;
    while (n < max && s->h->cmd_tail != s->h->cmd_head) {
        out[n++] = s->h->cmds[s->h->cmd_tail % HV_SHM_CMD_SLOTS];
        s->h->cmd_tail++;
        s->h->cmds_applied++;
    }
    unlock(s);
    return n;
}

void hv_shm_set_pointer(HvShm *s, int valid, int x, int y) {
    if (lock(s) != 0) return;
    s->h->ptr_valid = valid ? 1 : 0; s->h->ptr_x = x; s->h->ptr_y = y;
    unlock(s);
}

int hv_shm_get_pointer(HvShm *s, int *x, int *y) {
    if (lock(s) != 0) return -1;
    int v = s->h->ptr_valid;
    if (v) { *x = s->h->ptr_x; *y = s->h->ptr_y; }
    unlock(s);
    return v;
}

void hv_shm_set_state(HvShm *s, int state) { __atomic_store_n(&s->h->state, (uint32_t)state, __ATOMIC_RELEASE); }
void hv_shm_heartbeat(HvShm *s) { __atomic_fetch_add(&s->h->heartbeat, 1, __ATOMIC_RELAXED); }
void hv_shm_app_close(HvShm *s) { __atomic_store_n(&s->h->app_closed, 1u, __ATOMIC_RELEASE); }
int hv_shm_app_closed(HvShm *s) { return (int)__atomic_load_n(&s->h->app_closed, __ATOMIC_ACQUIRE); }

int hv_shm_stats(HvShm *s, HvShmStats *o) {
    memset(o, 0, sizeof *o);
    o->state = (int)__atomic_load_n(&s->h->state, __ATOMIC_ACQUIRE);
    o->runner_pid = __atomic_load_n(&s->h->runner_pid, __ATOMIC_ACQUIRE);
    o->heartbeat = __atomic_load_n(&s->h->heartbeat, __ATOMIC_RELAXED);
    if (lock(s) != 0) return -1;
    o->frames = s->h->frames; o->rejected = s->h->rejected;
    o->cmds_pushed = s->h->cmds_pushed; o->cmds_applied = s->h->cmds_applied; o->cmds_dropped = s->h->cmds_dropped;
    o->recovered = s->h->recovered; o->width = s->h->width; o->height = s->h->height;
    unlock(s);
    return 0;
}
