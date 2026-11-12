#define _GNU_SOURCE
#include "frame_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
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
    pthread_mutex_t mu;             /* process-shared, robust: guards everything below except the three atomics */
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
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    int ok = pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED) == 0 && pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST) == 0 && pthread_mutex_init(&h->mu, &a) == 0;
    pthread_mutexattr_destroy(&a);
    if (!ok) { munmap(s->h, s->len); close(fd); free(s); unlink(path); errno = EINVAL; return -1; }
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

/* 0 locked. -1 not locked (timeout or a broken mutex). A dead owner's lock is recovered: the picture is dropped, the count goes up. */
static int lock(HvShm *s) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += HV_SHM_LOCK_MS / 1000;
    ts.tv_nsec += (long)(HV_SHM_LOCK_MS % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    int r = pthread_mutex_timedlock(&s->h->mu, &ts);
    if (r == 0) return 0;
    if (r == EOWNERDEAD) {
        pthread_mutex_consistent(&s->h->mu);
        s->h->recovered++;
        s->h->width = s->h->height = 0;   /* the picture may be half written */
        return 0;
    }
    return -1;
}
static void unlock(HvShm *s) { pthread_mutex_unlock(&s->h->mu); }

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
