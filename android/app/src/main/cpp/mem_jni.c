#define _GNU_SOURCE

/* Guest-RAM tests for an Android app process. Henge maps guest RAM in
   devices/vmapple.c in one of these ways:
     anonymous   mmap(MAP_PRIVATE | MAP_ANON)                 (the default)
     file-backed ftruncate(file) + mmap(MAP_SHARED), file unlinked at once
                 (HV_RAM_FILE; the balloon punches holes in it)
   plus a third shape that exists only on Android: a memfd (shared memory).
   For each shape this checks what an app can actually do.

   Modes (mode argument):
     0  sparse: map 2, 4, 6 and 8 GiB, write and read back 256 pages spread
        over the mapping (first, middle and last included), for the fd shapes
        also read them back through the file descriptor, and punch one hole.
        Only the touched pages become real memory. A pass means "mapped and
        the sampled pages work", not "that much RAM can be used".
     1  dense: map 4 GiB, then touch every page in 256 MiB steps up to `mib`,
        and print memory counters after each step. This is the test that
        uses real memory; Android may kill the app under pressure. The step
        about to run is written to the progress file first, so a kill is
        attributable to a step, not explained.
     2  system information and a reserve-only probe of large anonymous sizes
        (mapped and unmapped at once, nothing touched).
     3  reclaim: the calls the engine's balloon uses on Apple, with their
        Android stand-ins. Anonymous: MADV_DONTNEED and MADV_FREE, each on its
        own 64 MiB range. memfd / file: fallocate(PUNCH_HOLE) + msync on 64 MiB.
        Each feature reports separately: the call (SUPPORTED / UNSUPPORTED),
        the data after it, and the memory counters. Memory counters are read
        BEFORE the range is touched again, because touching it allocates again.
        A drop in the counters is reported, never required: MADV_FREE keeps
        pages until the kernel wants them.

   Result states: PASS; MAP_FAILED (mmap said no); SETUP_FAILED (memfd_create,
   open or ftruncate said no; the call and errno are printed); UNSUPPORTED
   (mapping and samples fine, but a reclaim call is not available); SKIPPED
   (not enough free disk for a file-backed run); FAIL (wrong data); CANCELLED. */
#include <jni.h>

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/falloc.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define TAG "henge-jit-mem"
static size_t g_page = 4096; /* the device page size, read at the start of each call */
#define PAGE g_page
#define MiB (1ul << 20)
#define GiB (1ul << 30)
#define SAMPLES 256ul

#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 0x01
#endif
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE 0x02
#endif

extern atomic_int g_cancel; /* mt_jni.c: set by the cancel button, cleared before each run */

enum { T_ANON = 0, T_MEMFD = 1, T_FILE = 2 };
enum { R_PASS = 0, R_MAPFAIL, R_SKIPPED, R_FAIL, R_CANCELLED, R_SETUPFAIL, R_UNSUPPORTED };

static const char *const TYPE_NAME[] = {"anonymous (MAP_PRIVATE|MAP_ANON)", "memfd (shared memory)",
                                        "file in app storage (MAP_SHARED, unlinked)"};

/* ---- failure records ---- */

static char g_step_buf[160];
static const char *volatile g_step = "idle";
static volatile int g_crash_fd = -1;
static int g_progress_fd = -1;

static void put_str(char *buf, int *n, int cap, const char *s) {
    while (*s && *n < cap - 1) buf[(*n)++] = *s++;
}

static void put_num(char *buf, int *n, int cap, unsigned long v, int base) {
    char tmp[24];
    int i = 0;
    do {
        tmp[i++] = "0123456789abcdef"[v % (unsigned)base];
        v /= (unsigned)base;
    } while (v && i < (int)sizeof tmp);
    while (i && *n < cap - 1) buf[(*n)++] = tmp[--i];
}

static void on_fatal(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    char buf[240];
    int n = 0;
    put_str(buf, &n, sizeof buf, "CRASH: signal ");
    put_num(buf, &n, sizeof buf, (unsigned long)sig, 10);
    put_str(buf, &n, sizeof buf, sig == SIGSEGV ? " (SEGV)" : sig == SIGBUS ? " (BUS)" : sig == SIGILL ? " (ILL)" : "");
    put_str(buf, &n, sizeof buf, " during step: ");
    put_str(buf, &n, sizeof buf, g_step);
    put_str(buf, &n, sizeof buf, ", fault address 0x");
    put_num(buf, &n, sizeof buf, (unsigned long)si->si_addr, 16);
    put_str(buf, &n, sizeof buf, "\n");
    if (g_crash_fd >= 0) (void)!write(g_crash_fd, buf, (size_t)n);
    _exit(128 + sig);
}

static void mark(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_step_buf, sizeof g_step_buf, fmt, ap);
    va_end(ap);
    g_step = g_step_buf;
    if (g_progress_fd >= 0) {
        char line[200];
        int n = snprintf(line, sizeof line, "memory test: %s\n", g_step_buf);
        if (n > 0 && pwrite(g_progress_fd, line, (size_t)n, 0) == n) (void)!ftruncate(g_progress_fd, n);
    }
}

static void addf(char *out, size_t cap, const char *fmt, ...) {
    size_t used = strlen(out);
    if (used >= cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + used, cap - used, fmt, ap);
    va_end(ap);
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- memory counters (kB, from /proc) ---- */

typedef struct {
    long vmsize, rss, anon, file, shmem; /* of this process */
    long total, avail;                   /* of the device */
} Mem;

static long field_kb(const char *text, const char *key) {
    const char *p = strstr(text, key);
    return p ? strtol(p + strlen(key), NULL, 10) : -1;
}

static void read_mem(Mem *m) {
    char buf[4096];
    memset(m, 0, sizeof *m);
    m->vmsize = m->rss = m->anon = m->file = m->shmem = m->total = m->avail = -1;
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        fclose(f);
        m->vmsize = field_kb(buf, "VmSize:");
        m->rss = field_kb(buf, "VmRSS:");
        m->anon = field_kb(buf, "RssAnon:");
        m->file = field_kb(buf, "RssFile:");
        m->shmem = field_kb(buf, "RssShmem:");
    }
    f = fopen("/proc/meminfo", "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        fclose(f);
        m->total = field_kb(buf, "MemTotal:");
        m->avail = field_kb(buf, "MemAvailable:");
    }
}

#define MB(kb) ((kb) < 0 ? -1L : (kb) / 1024)

static void print_mem(char *out, size_t cap, const char *label, const Mem *m) {
    addf(out, cap, "  %s: RSS %ld MiB (anon %ld, file %ld, shmem %ld); device available %ld of %ld MiB\n", label, MB(m->rss),
         MB(m->anon), MB(m->file), MB(m->shmem), MB(m->avail), MB(m->total));
}

/* ---- one mapping ---- */

typedef struct {
    uint8_t *p;
    size_t size;
    int fd;
    int type;
} Map;

/* 0: mapped; 1: mmap failed; 2: memfd_create / open / ftruncate failed (the call is named in the output). */
static int map_open(Map *m, int type, size_t size, const char *dir, char *out, size_t cap) {
    m->p = NULL;
    m->size = size;
    m->fd = -1;
    m->type = type;
    void *p = MAP_FAILED;
    int err = 0;
    const char *step = "mmap";

    if (type == T_ANON) {
        mark("anonymous: mmap %zu MiB", size / MiB);
        p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        err = errno;
    } else {
        if (type == T_MEMFD) {
            mark("memfd: memfd_create");
            m->fd = memfd_create("henge-guest-ram", MFD_CLOEXEC);
            step = "memfd_create";
        } else {
            char path[512];
            snprintf(path, sizeof path, "%s/guest_ram.bin", dir);
            mark("file: open %s", path);
            m->fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (m->fd >= 0) unlink(path); /* as the engine does: it goes away with the process */
            step = "open";
        }
        err = errno;
        if (m->fd >= 0) {
            mark("%s: ftruncate %zu MiB", type == T_MEMFD ? "memfd" : "file", size / MiB);
            if (ftruncate(m->fd, (off_t)size) != 0) {
                err = errno;
                step = "ftruncate";
            } else {
                mark("%s: mmap MAP_SHARED %zu MiB", type == T_MEMFD ? "memfd" : "file", size / MiB);
                p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd, 0);
                err = errno;
                step = "mmap";
            }
        }
    }
    if (p == MAP_FAILED) {
        addf(out, cap, "  %s failed: %s (errno %d)\n", step, strerror(err), err);
        if (m->fd >= 0) close(m->fd);
        m->fd = -1;
        return strcmp(step, "mmap") == 0 ? 1 : 2;
    }
    m->p = p;
    return 0;
}

static void map_close(Map *m) {
    if (m->p) munmap(m->p, m->size);
    if (m->fd >= 0) close(m->fd);
    m->p = NULL;
    m->fd = -1;
}

static uint64_t pattern(size_t page) { return 0xC0FFEE0000000000ull ^ (uint64_t)page * 0x9E3779B97F4A7C15ull; }

static int cancelled(void) { return atomic_load_explicit(&g_cancel, memory_order_relaxed); }

/* ---- mode 0 ---- */

static int run_sparse(int type, size_t gib, const char *dir, char *out, size_t cap) {
    size_t size = gib * GiB;
    Map m;
    Mem before, after;

    if (type == T_FILE) {
        struct statvfs vfs;
        if (statvfs(dir, &vfs) == 0)
            addf(out, cap, "  app storage free: %llu MiB (the file is sparse; only touched pages use space)\n",
                 (unsigned long long)((uint64_t)vfs.f_bavail * vfs.f_frsize / MiB));
    }
    read_mem(&before);
    int mo = map_open(&m, type, size, dir, out, cap);
    if (mo) return mo == 1 ? R_MAPFAIL : R_SETUPFAIL;
    addf(out, cap, "  mapped %zu GiB at %p\n", gib, (void *)m.p);
    if (cancelled()) {
        map_close(&m);
        return R_CANCELLED;
    }

    size_t pages = size / PAGE;
    size_t n = pages < SAMPLES ? pages : SAMPLES;
    size_t stride = pages / n;
    size_t bad = 0, bad_fd = 0, fd_checked = 0;

    mark("%s %zu GiB: write %zu sample pages", type == T_ANON ? "anonymous" : type == T_MEMFD ? "memfd" : "file", gib, n);
    for (size_t i = 0; i < n; i++) {
        size_t pg = i == n - 1 ? pages - 1 : i * stride; /* first ... last page included */
        *(volatile uint64_t *)(m.p + pg * PAGE + 8) = pattern(pg);
    }
    mark("%s %zu GiB: read the sample pages back", type == T_ANON ? "anonymous" : type == T_MEMFD ? "memfd" : "file", gib);
    for (size_t i = 0; i < n; i++) {
        size_t pg = i == n - 1 ? pages - 1 : i * stride;
        if (*(volatile uint64_t *)(m.p + pg * PAGE + 8) != pattern(pg)) bad++;
        if (m.fd >= 0 && (i < 8 || i == n - 1 || i == n / 2)) { /* also through the descriptor: the file sees the write */
            uint64_t v = 0;
            fd_checked++;
            if (pread(m.fd, &v, sizeof v, (off_t)(pg * PAGE + 8)) != (ssize_t)sizeof v || v != pattern(pg)) bad_fd++;
        }
    }
    if (cancelled()) {
        map_close(&m);
        return R_CANCELLED;
    }
    addf(out, cap, "  sampled %zu pages (of %zu): %zu wrong through the mapping", n, pages, bad);
    if (m.fd >= 0) addf(out, cap, "; %zu of %zu wrong through the file descriptor", bad_fd, fd_checked);
    addf(out, cap, "\n");

    int hole_fail = 0, hole_unsupported = 0;
    if (cancelled()) {
        map_close(&m);
        return R_CANCELLED;
    }
    if (m.fd >= 0) { /* the balloon frees guest pages this way; the full test is mode 3 */
        size_t pg = n > 8 ? 8 * stride : 0;
        mark("punch a hole at page %zu", pg);
        if (fallocate(m.fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)(pg * PAGE), (off_t)PAGE) != 0) {
            addf(out, cap, "  punch hole: UNSUPPORTED (%s, errno %d)\n", strerror(errno), errno);
            hole_unsupported = 1;
        } else {
            uint64_t v = *(volatile uint64_t *)(m.p + pg * PAGE + 8);
            addf(out, cap, "  punch hole: SUPPORTED, page now reads %s\n", v == 0 ? "zero" : "NOT zero (DATA_ERROR)");
            hole_fail = v != 0;
        }
    }
    read_mem(&after);
    print_mem(out, cap, "before", &before);
    print_mem(out, cap, "after ", &after);
    addf(out, cap, "  (VmSize grew by %ld MiB; RSS only by what was touched)\n", MB(after.vmsize - before.vmsize));
    mark("unmap");
    map_close(&m);
    if (bad || bad_fd || hole_fail) return R_FAIL;
    return hole_unsupported ? R_UNSUPPORTED : R_PASS;
}

/* ---- mode 1 ---- */

static int run_dense(int type, size_t cap_mib, const char *dir, char *out, size_t cap) {
    size_t size = 4 * GiB;
    size_t step_mib = 256;
    Map m;
    Mem mm;

    if (type == T_FILE) { /* touched pages of a file use real space */
        struct statvfs vfs;
        uint64_t freemb = statvfs(dir, &vfs) == 0 ? (uint64_t)vfs.f_bavail * vfs.f_frsize / MiB : 0;
        if (freemb < cap_mib + 1024) {
            addf(out, cap, "  skipped: %llu MiB free in app storage, need %zu MiB plus 1 GiB spare\n",
                 (unsigned long long)freemb, cap_mib);
            return R_SKIPPED;
        }
    }
    int mo = map_open(&m, type, size, dir, out, cap);
    if (mo) return mo == 1 ? R_MAPFAIL : R_SETUPFAIL;
    read_mem(&mm);
    print_mem(out, cap, "start ", &mm);

    int bad = 0;
    for (size_t done = 0; done < cap_mib; done += step_mib) {
        mark("%s: touching %zu..%zu MiB", type == T_ANON ? "anonymous" : type == T_MEMFD ? "memfd" : "file", done,
             done + step_mib);
        long long t0 = now_ms();
        size_t first = done * MiB / PAGE, last = (done + step_mib) * MiB / PAGE;
        for (size_t pg = first; pg < last; pg++) {
            *(volatile uint64_t *)(m.p + pg * PAGE + 8) = pattern(pg);
            if ((pg & 4095) == 0 && cancelled()) { /* every 16 MiB */
                map_close(&m);
                addf(out, cap, "  cancelled at about %zu MiB\n", pg * PAGE / MiB);
                return R_CANCELLED;
            }
        }
        long long t1 = now_ms();
        read_mem(&mm);
        char label[64];
        snprintf(label, sizeof label, "touched %4zu MiB (%lld ms)", done + step_mib, t1 - t0);
        print_mem(out, cap, label, &mm);
    }
    mark("verify touched pages");
    size_t first_bad = (size_t)-1, nbad = 0, total = cap_mib * MiB / PAGE;
    for (size_t pg = 0; pg < total; pg++)
        if (*(volatile uint64_t *)(m.p + pg * PAGE + 8) != pattern(pg)) {
            if (!nbad) first_bad = pg;
            nbad++;
        }
    addf(out, cap, "  verified %zu touched pages: %zu wrong\n", total, nbad);
    if (nbad) addf(out, cap, "  first wrong page %zu\n", first_bad);
    bad = nbad != 0;
    mark("unmap");
    map_close(&m);
    read_mem(&mm);
    print_mem(out, cap, "after unmap", &mm);
    return bad ? R_FAIL : R_PASS;
}

/* ---- mode 3 ---- */

#define RANGE_MIB 64ul

static long lazyfree_kb(void) { /* -1: smaps_rollup not readable */
    char buf[2048];
    FILE *f = fopen("/proc/self/smaps_rollup", "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return field_kb(buf, "LazyFree:");
}

static void touch_range(uint8_t *base, size_t first_page, size_t pages) {
    for (size_t i = 0; i < pages; i++) *(volatile uint64_t *)(base + (first_page + i) * PAGE + 8) = pattern(first_page + i);
}

/* Reads the range back and sorts every page into: still the pattern, zero, other. */
static void classify_range(const uint8_t *base, size_t first_page, size_t pages, size_t *kept, size_t *zero, size_t *other) {
    *kept = *zero = *other = 0;
    for (size_t i = 0; i < pages; i++) {
        uint64_t v = *(volatile const uint64_t *)(base + (first_page + i) * PAGE + 8);
        if (v == pattern(first_page + i)) (*kept)++;
        else if (v == 0) (*zero)++;
        else (*other)++;
    }
}

static void print_range_mem(char *out, size_t cap, const char *label, const Mem *m, long lazy) {
    addf(out, cap, "    %-8s RSS %ld MiB (anon %ld, file %ld, shmem %ld), LazyFree %s", label, MB(m->rss), MB(m->anon),
         MB(m->file), MB(m->shmem), "");
    if (lazy == -2) addf(out, cap, "n/a\n");
    else if (lazy < 0) addf(out, cap, "unavailable\n");
    else addf(out, cap, "%ld MiB\n", lazy / 1024);
}

/* madvise one 64 MiB range; dont_need selects MADV_DONTNEED (zero on re-read expected), else MADV_FREE (anything goes). */
static int reclaim_anon(Map *m, size_t first_page, int advice, const char *name, char *out, size_t cap, int *unsupported) {
    size_t pages = RANGE_MIB * MiB / PAGE, kept, zero, other;
    Mem a, b;
    mark("reclaim %s: touch %lu MiB", name, RANGE_MIB);
    touch_range(m->p, first_page, pages);
    read_mem(&a);
    long la = lazyfree_kb();
    mark("reclaim %s: madvise", name);
    int rc = madvise(m->p + first_page * PAGE, pages * PAGE, advice);
    int err = errno;
    read_mem(&b); /* before the range is touched again */
    long lb = lazyfree_kb();
    addf(out, cap, "  %s on %lu MiB: ", name, RANGE_MIB);
    if (rc != 0) {
        addf(out, cap, "UNSUPPORTED (%s, errno %d)\n", strerror(err), err);
        *unsupported = 1;
        return 0;
    }
    addf(out, cap, "call SUPPORTED\n");
    print_range_mem(out, cap, "before:", &a, la);
    print_range_mem(out, cap, "after:", &b, lb);
    mark("reclaim %s: read back", name);
    classify_range(m->p, first_page, pages, &kept, &zero, &other);
    addf(out, cap, "    re-read: %zu still the old data, %zu zero, %zu other\n", kept, zero, other);
    if (advice == MADV_DONTNEED && (kept || other)) {
        addf(out, cap, "    DATA_ERROR: MADV_DONTNEED on private anonymous memory should read back zero\n");
        return 1;
    }
    if (advice != MADV_DONTNEED && other) {
        addf(out, cap, "    DATA_ERROR: neither the old data nor zero\n");
        return 1;
    }
    if (advice != MADV_DONTNEED) addf(out, cap, "    (observation: MADV_FREE may keep the data until the kernel needs the pages)\n");
    return 0;
}

static int run_reclaim(int type, const char *dir, char *out, size_t cap) {
    Map m;
    int bad = 0, unsupported = 0;
    size_t range_pages = RANGE_MIB * MiB / PAGE;
    int mo = map_open(&m, type, 2 * RANGE_MIB * MiB, dir, out, cap);
    if (mo) return mo == 1 ? R_MAPFAIL : R_SETUPFAIL;

    if (type == T_ANON) {
        bad |= reclaim_anon(&m, 0, MADV_DONTNEED, "MADV_DONTNEED", out, cap, &unsupported);
        if (cancelled()) goto cancel;
#ifdef MADV_FREE
        bad |= reclaim_anon(&m, range_pages, MADV_FREE, "MADV_FREE", out, cap, &unsupported);
#else
        addf(out, cap, "  MADV_FREE: UNSUPPORTED (not defined)\n"), unsupported = 1;
#endif
    } else {
        size_t kept, zero, other;
        Mem a, b;
        mark("reclaim hole: touch %lu MiB", RANGE_MIB);
        touch_range(m.p, 0, range_pages);
        read_mem(&a);
        if (cancelled()) goto cancel;
        mark("reclaim hole: fallocate PUNCH_HOLE");
        int rc = fallocate(m.fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, (off_t)(range_pages * PAGE));
        int err = errno;
        if (rc != 0) {
            addf(out, cap, "  fallocate(PUNCH_HOLE) on %lu MiB: UNSUPPORTED (%s, errno %d)\n", RANGE_MIB, strerror(err), err);
            unsupported = 1;
        } else {
            addf(out, cap, "  fallocate(PUNCH_HOLE) on %lu MiB: call SUPPORTED\n", RANGE_MIB);
            mark("reclaim hole: msync MS_INVALIDATE");
            int mrc = msync(m.p, range_pages * PAGE, MS_INVALIDATE); /* the engine does this after a successful punch */
            int merr = errno;
            addf(out, cap, "  msync(MS_INVALIDATE): %s", mrc == 0 ? "ok\n" : "");
            if (mrc != 0) addf(out, cap, "failed (%s, errno %d)\n", strerror(merr), merr);
            read_mem(&b); /* before the range is read again */
            print_range_mem(out, cap, "before:", &a, -2);
            print_range_mem(out, cap, "after:", &b, -2);
            mark("reclaim hole: read back");
            classify_range(m.p, 0, range_pages, &kept, &zero, &other);
            addf(out, cap, "    re-read: %zu still the old data, %zu zero, %zu other\n", kept, zero, other);
            if (kept || other) {
                addf(out, cap, "    DATA_ERROR: a punched range must read back zero\n");
                bad = 1;
            }
        }
    }
    map_close(&m);
    if (bad) return R_FAIL;
    return unsupported ? R_UNSUPPORTED : R_PASS;
cancel:
    map_close(&m);
    return R_CANCELLED;
}

/* ---- mode 2 ---- */

static void run_info(const char *dir, char *out, size_t cap) {
    Mem m;
    read_mem(&m);
    print_mem(out, cap, "now", &m);
    struct rlimit rl;
    if (getrlimit(RLIMIT_AS, &rl) == 0) {
        if (rl.rlim_cur == RLIM_INFINITY) addf(out, cap, "  RLIMIT_AS: unlimited\n");
        else addf(out, cap, "  RLIMIT_AS: %llu MiB\n", (unsigned long long)rl.rlim_cur / MiB);
    }
    struct statvfs vfs;
    if (statvfs(dir, &vfs) == 0)
        addf(out, cap, "  app storage free: %llu MiB\n", (unsigned long long)((uint64_t)vfs.f_bavail * vfs.f_frsize / MiB));
    FILE *f = fopen("/proc/sys/vm/overcommit_memory", "r");
    if (f) {
        int v = -1;
        if (fscanf(f, "%d", &v) == 1) addf(out, cap, "  vm.overcommit_memory: %d\n", v);
        fclose(f);
    } else {
        addf(out, cap, "  vm.overcommit_memory: not readable\n");
    }
    static const size_t probe_gib[] = {8, 12, 16, 24, 32};
    addf(out, cap, "  reserve-only probe (anonymous, mapped and unmapped at once, nothing touched):");
    for (size_t i = 0; i < sizeof probe_gib / sizeof *probe_gib; i++) {
        mark("reserve probe %zu GiB", probe_gib[i]);
        void *p = mmap(NULL, probe_gib[i] * GiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) addf(out, cap, " %zu GiB:%s", probe_gib[i], strerror(errno));
        else {
            addf(out, cap, " %zu GiB:ok", probe_gib[i]);
            munmap(p, probe_gib[i] * GiB);
        }
    }
    addf(out, cap, "\n");
}

JNIEXPORT jstring JNICALL Java_dev_muns_hengeprobe_MainActivity_runMem(JNIEnv *env, jobject self, jint mode, jint type,
                                                                       jint size, jstring files_dir, jstring crash_path,
                                                                       jstring progress_path) {
    (void)self;
    char out[3072] = "";
    const char *dir = (*env)->GetStringUTFChars(env, files_dir, NULL);
    const char *cpath = (*env)->GetStringUTFChars(env, crash_path, NULL);
    const char *ppath = (*env)->GetStringUTFChars(env, progress_path, NULL);
    g_crash_fd = open(cpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    g_progress_fd = open(ppath, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);

    struct sigaction sa, old_segv, old_bus, old_ill;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fatal;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);
    sigaction(SIGILL, &sa, &old_ill);

    int st = R_PASS;
    long ps = sysconf(_SC_PAGESIZE);
    if (ps > 0) g_page = (size_t)ps;
    if (type < 0 || type > 2) type = 0;
    if (cancelled()) {
        st = R_CANCELLED;
        addf(out, sizeof out, "  cancelled before start\n");
    } else if (mode == 2) {
        addf(out, sizeof out, "== system ==\n");
        run_info(dir, out, sizeof out);
    } else if (mode == 3) {
        addf(out, sizeof out, "== reclaim: %s ==\n", TYPE_NAME[type]);
        st = run_reclaim(type, dir, out, sizeof out);
    } else if (mode == 1) {
        addf(out, sizeof out, "== dense: %s, up to %d MiB ==\n", TYPE_NAME[type], (int)size);
        st = run_dense(type, (size_t)size, dir, out, sizeof out);
    } else {
        addf(out, sizeof out, "== sparse: %s, %d GiB ==\n", TYPE_NAME[type], (int)size);
        st = run_sparse(type, (size_t)size, dir, out, sizeof out);
    }

    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);
    sigaction(SIGILL, &old_ill, NULL);
    if (g_crash_fd >= 0) close(g_crash_fd);
    g_crash_fd = -1;
    unlink(cpath);
    if (g_progress_fd >= 0) close(g_progress_fd);
    g_progress_fd = -1;
    (*env)->ReleaseStringUTFChars(env, files_dir, dir);
    (*env)->ReleaseStringUTFChars(env, crash_path, cpath);
    (*env)->ReleaseStringUTFChars(env, progress_path, ppath);

    static const char *const state[] = {"PASS", "MAP_FAILED", "SKIPPED", "FAIL", "CANCELLED", "SETUP_FAILED", "UNSUPPORTED"};
    if (mode == 3) addf(out, sizeof out, "RESULT: %s (reclaim)\n", state[st]);
    else if (mode != 2) addf(out, sizeof out, "RESULT: %s (%s %d %s)\n", state[st], mode == 1 ? "dense" : "sparse", (int)size,
                        mode == 1 ? "MiB" : "GiB");
    __android_log_print(st == R_PASS ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, TAG, "%s", out);
    return (*env)->NewStringUTF(env, out);
}
