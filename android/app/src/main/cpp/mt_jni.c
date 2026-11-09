#define _GNU_SOURCE /* pipe2, sched_setaffinity, CPU_* */

/* Multi-thread test for patching a branch in the split JIT mapping while other
   threads run it. This is the shape of Henge's cross-page block chaining
   (core/dispatch.c chain()): one thread rewrites a 4-byte B through the RW
   alias with hv_jit_write() and then calls hv_jit_finish(); other threads keep
   running through that branch at the RX address. The engine's own functions
   are used unchanged (host/jit_region.c).

   Code layout in a 1 MiB region (word = 4 bytes):
     word 0   b <A or B>        the patched site; executors call this address
     word 16  mov x0,#1 ; ret   destination A
     word 32  mov x0,#2 ; ret   destination B
   An executor therefore gets 1 or 2. Anything else is a failure.

   Stages (stage argument):
     1  one executor thread, patching all the time
     2  several executor threads (all other CPUs), patching all the time
     3  visibility, no synchronisation on the executor side ("A"): executors
        keep running the site; after each patch we measure the time from the
        return of hv_jit_finish() until an executor first returns the new
        destination. The engine does exactly this: it does no executor-side
        sync after chain patches.
     4  visibility with an explicit sync ("B"): executors wait, then on a new
        patch run the site once without sync (counted as "stale" if it still
        shows the old destination, informational) and once after an ISB (a
        wrong result here is a violation). This tests a protocol the engine
        does not use today; it shows what would be needed if stage 3 looked bad.
     5  long run: stage 2 for `seconds` seconds

   Result states, in priority order: FAIL (a value other than 1 or 2, a setup
   error, a thread that could not start), CANCELLED (the user pressed cancel),
   INCOMPLETE (an executor did not answer within the 500 ms observation limit,
   or the stage hit its overall time cap; that is a missed deadline, not proof
   that a thread is stuck), UNPINNED (stages 3 and 4 could not pin their
   threads to separate CPUs, so the cross-CPU condition was not met), PASS.
   Counters are kept separately from the state.

   What a pass means: on this device, in this process, with this patch shape,
   no invalid value, crash or stall was seen. It does not prove ARM64 gives
   such a guarantee. For stage 4, PASS only means the observation run
   finished; "old destination after ISB" is reported as an observation, not a
   verdict. The time limits and cancel only work while control flow is normal;
   a thread that jumps somewhere wrong and never returns is not stopped by
   them (force-stop the app), and the signal record only covers SEGV, BUS and
   ILL.

   Stage 3 limitation: the patch is written before the generation number is
   published, so a call can run new code while the executor still reads the old
   generation. The destinations alternate, so such a call does not match the
   old generation's expected value and is not counted; the next call is. */
#include <jni.h>

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "jit_region.h"

#define TAG "henge-jit-mt"
#define MAX_EXEC 8
#define MAX_EPOCHS 2000
#define SITE 0u
#define BLOCK_A 16u
#define BLOCK_B 32u
#define OBSERVE_LIMIT_NS (500LL * 1000 * 1000)
#define STAGE_CAP_NS (20LL * 1000 * 1000 * 1000)
#define HOLD_NS (1000LL * 1000)

typedef long (*jit_fn)(void);

static const uint32_t RET = 0xd65f03c0;

/* Set from the UI thread (cancelMt), cleared once per run of stages (beginMt). */
atomic_int g_cancel; /* also read by mem_jni.c */
static int cancelled(void) { return atomic_load_explicit(&g_cancel, memory_order_relaxed); }

enum { R_PASS = 0, R_FAIL, R_CANCELLED, R_INCOMPLETE, R_UNPINNED };

static uint32_t mov_x0(unsigned imm) { return 0xd2800000u | (imm << 5); }
static uint32_t branch_to(unsigned word) { return 0x14000000u | (word & 0x3ffffffu); } /* site is word 0 */

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void sleep_ns(long long ns) {
    struct timespec ts = {(time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL)};
    nanosleep(&ts, NULL);
}

static inline void sync_instructions(void) {
#if defined(__aarch64__)
    __asm__ volatile("isb" ::: "memory");
#else
    atomic_thread_fence(memory_order_seq_cst);
#endif
}

static inline void cpu_relax(void) {
#if defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#endif
}

/* ---- failure records (same scheme as probe_jni.c) ---- */

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

static void mark(int stage, const char *step) {
    snprintf(g_step_buf, sizeof g_step_buf, "multi-thread stage %d: %s", stage, step);
    g_step = g_step_buf;
    if (g_progress_fd >= 0) {
        char line[200];
        int n = snprintf(line, sizeof line, "%s\n", g_step_buf);
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

/* ---- the test ---- */

enum { M_CONCURRENT = 0, M_VIS_A = 1, M_VIS_B = 2 };

typedef struct Shared Shared;

typedef struct {
    Shared *s;
    int id;
    int cpu; /* -1: not pinned */
    int pin_ok;
    int started;
    pthread_t th;
    uint64_t calls, na, nb, invalid;
    long first_invalid;
    uint64_t old_before, old_after, moved; /* stage 4: old destination before / after the ISB; calls skipped because the generation moved */
    uint32_t nlat;
    uint32_t lat_us[MAX_EPOCHS]; /* stage 3 */
    atomic_int last_seen;
} Exec;

struct Shared {
    HvJitRegion *r;
    int mode;
    int n;
    atomic_int stop;
    atomic_int epoch;
    atomic_llong t0;
    Exec ex[MAX_EXEC];
};

/* Patch number e (1-based): odd goes to A, even to B. Patch 0 is the initial state (B). */
static long want(int e) { return (e & 1) ? 1 : 2; }

static void patch(HvJitRegion *r, int e) {
    uint32_t insn = branch_to((e & 1) ? BLOCK_A : BLOCK_B);
    hv_jit_write(r, r->base + SITE, &insn, sizeof insn);
    hv_jit_finish(r, r->base + SITE, 1);
}

static void *exec_main(void *p) {
    Exec *x = p;
    Shared *s = x->s;
    jit_fn fn = (jit_fn)(void *)(s->r->base + SITE);

    if (x->cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(x->cpu, &set);
        x->pin_ok = sched_setaffinity(0, sizeof set, &set) == 0;
    }

    if (s->mode == M_CONCURRENT) {
        while (!atomic_load_explicit(&s->stop, memory_order_relaxed)) {
            long r = fn();
            x->calls++;
            if (r == 1) x->na++;
            else if (r == 2) x->nb++;
            else {
                if (!x->invalid) x->first_invalid = r;
                x->invalid++;
            }
        }
    } else if (s->mode == M_VIS_A) {
        int last = 0;
        while (!atomic_load_explicit(&s->stop, memory_order_relaxed)) {
            /* The generation is read before and after the call; only a call that
               started after the patch was published and ended in the same generation counts. */
            int e1 = atomic_load_explicit(&s->epoch, memory_order_acquire);
            long r = fn();
            int e2 = atomic_load_explicit(&s->epoch, memory_order_acquire);
            x->calls++;
            if (r != 1 && r != 2) {
                if (!x->invalid) x->first_invalid = r;
                x->invalid++;
            } else if (e1 == e2 && e1 > last && r == want(e1)) {
                long long dt = now_ns() - atomic_load_explicit(&s->t0, memory_order_relaxed);
                if (dt < 0) dt = 0;
                if (x->nlat < MAX_EPOCHS) x->lat_us[x->nlat++] = (uint32_t)(dt / 1000);
                last = e1;
                atomic_store_explicit(&x->last_seen, e1, memory_order_release);
            }
        }
    } else {
        int last = 0;
        while (!atomic_load_explicit(&s->stop, memory_order_relaxed)) {
            int e = atomic_load_explicit(&s->epoch, memory_order_acquire);
            if (e == last) {
                cpu_relax();
                continue;
            }
            long r0 = fn(); /* no synchronisation */
            sync_instructions();
            long r1 = fn(); /* after the ISB */
            /* The patch is written before the generation is published, so an unchanged
               generation does not prove no patch happened; this only reduces the noise. */
            int moved = atomic_load_explicit(&s->epoch, memory_order_acquire) != e;
            x->calls += 2;
            if (r0 != 1 && r0 != 2) {
                if (!x->invalid) x->first_invalid = r0;
                x->invalid++;
            } else if (r0 != want(e)) x->old_before++;
            if (r1 != 1 && r1 != 2) {
                if (!x->invalid) x->first_invalid = r1;
                x->invalid++;
            } else if (r1 != want(e)) {
                if (moved) x->moved++;
                else x->old_after++;
            }
            last = e;
            atomic_store_explicit(&x->last_seen, e, memory_order_release);
        }
    }
    return NULL;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int run_stage(int stage, int seconds, char *out, size_t cap) {
    static Shared sh; /* large; one stage runs at a time */
    HvJitRegion region;
    int fail = 0, incomplete = 0, unpinned = 0;

    if (cancelled()) {
        addf(out, cap, "  cancelled before start\n");
        return R_CANCELLED;
    }

    mark(stage, "open region");
    if (hv_jit_region_open(&region, (size_t)1 << 20) != 0 || !region.rw_diff) {
        addf(out, cap, "  FAIL: no split JIT region (errno %d: %s)\n", errno, strerror(errno));
        return R_FAIL;
    }

    memset(&sh, 0, sizeof sh);
    sh.r = &region;
    sh.mode = stage == 3 ? M_VIS_A : stage == 4 ? M_VIS_B : M_CONCURRENT;

    /* CPUs we may run on: the patcher takes the last one, executors the rest. */
    int cpus[CPU_SETSIZE], ncpu = 0;
    cpu_set_t avail;
    CPU_ZERO(&avail);
    if (sched_getaffinity(0, sizeof avail, &avail) == 0)
        for (int c = 0; c < CPU_SETSIZE; c++)
            if (CPU_ISSET(c, &avail)) cpus[ncpu++] = c;
    int want_exec = stage == 1 ? 1 : (stage == 3 || stage == 4) ? 2 : 4;
    int n_exec = want_exec;
    if (n_exec > MAX_EXEC) n_exec = MAX_EXEC;
    int pin = (stage == 3 || stage == 4) && ncpu >= 2;
    int patcher_cpu = ncpu ? cpus[ncpu - 1] : -1;
    sh.n = n_exec;

    /* Code: the site branches to B first; blocks A and B return 1 and 2. */
    uint32_t a[2] = {mov_x0(1), RET}, b[2] = {mov_x0(2), RET};
    hv_jit_write(&region, region.base + BLOCK_A, a, sizeof a);
    hv_jit_write(&region, region.base + BLOCK_B, b, sizeof b);
    hv_jit_finish(&region, region.base + BLOCK_A, 2);
    hv_jit_finish(&region, region.base + BLOCK_B, 2);
    patch(&region, 0);
    long first = ((jit_fn)(void *)(region.base + SITE))();
    if (first != 2) {
        addf(out, cap, "  FAIL: initial run returned %ld (expect 2)\n", first);
        hv_jit_region_close(&region);
        return R_FAIL;
    }

    mark(stage, "start executor threads");
    int create_fail = 0;
    for (int i = 0; i < n_exec; i++) {
        Exec *x = &sh.ex[i];
        x->s = &sh;
        x->id = i;
        x->cpu = pin ? cpus[i % (ncpu - 1)] : -1;
        atomic_init(&x->last_seen, 0);
        int rc = pthread_create(&x->th, NULL, exec_main, x); /* returns the error number, errno is not set */
        if (rc == 0) {
            x->started = 1;
        } else {
            addf(out, cap, "  FAIL: could not start executor %d: %s\n", i, strerror(rc));
            create_fail = 1;
            break;
        }
    }
    if (create_fail) { /* stop and join those that did start, then give up on this stage */
        atomic_store_explicit(&sh.stop, 1, memory_order_release);
        for (int i = 0; i < n_exec; i++)
            if (sh.ex[i].started) pthread_join(sh.ex[i].th, NULL);
        hv_jit_region_close(&region);
        return R_FAIL;
    }
    sleep_ns(30LL * 1000 * 1000);

    int pin_patcher = 0;
    if (pin) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(patcher_cpu, &set);
        pin_patcher = sched_setaffinity(0, sizeof set, &set) == 0;
    }

    uint64_t patches = 0, misses = 0;
    int timecap = 0;
    long long began = now_ns();

    if (sh.mode == M_CONCURRENT) {
        mark(stage, "patching while executors run");
        long long end = began + (long long)seconds * 1000000000LL;
        int e = 0;
        while (now_ns() < end && !cancelled()) {
            for (int k = 0; k < 256; k++) { /* check the clock and cancel only now and then */
                e++;
                patch(&region, e);
                patches++;
            }
            atomic_store_explicit(&sh.epoch, e, memory_order_release);
        }
    } else {
        mark(stage, sh.mode == M_VIS_A ? "visibility without executor sync" : "visibility with ISB on the executor");
        for (int e = 1; e <= MAX_EPOCHS && !cancelled(); e++) {
            if (now_ns() - began > STAGE_CAP_NS) {
                timecap = 1;
                break;
            }
            patch(&region, e);
            patches++;
            long long t0 = now_ns();
            atomic_store_explicit(&sh.t0, t0, memory_order_relaxed);
            atomic_store_explicit(&sh.epoch, e, memory_order_release);
            for (;;) {
                int unseen = 0;
                for (int i = 0; i < n_exec; i++)
                    if (sh.ex[i].started && atomic_load_explicit(&sh.ex[i].last_seen, memory_order_acquire) < e) unseen++;
                if (!unseen || cancelled()) break;
                if (now_ns() - t0 > OBSERVE_LIMIT_NS) {
                    misses += (uint64_t)unseen;
                    break;
                }
                cpu_relax();
            }
            sleep_ns(HOLD_NS); /* keep the new destination for a while */
        }
    }
    double secs = (double)(now_ns() - began) / 1e9;

    mark(stage, "stop and join executors");
    atomic_store_explicit(&sh.stop, 1, memory_order_release);
    for (int i = 0; i < n_exec; i++)
        if (sh.ex[i].started) pthread_join(sh.ex[i].th, NULL);
    if (pin_patcher) sched_setaffinity(0, sizeof avail, &avail); /* give this thread its CPUs back */

    addf(out, cap, "  CPUs usable %d (%d..%d); %d executors%s, patcher %s; %.1f s, %llu patches\n", ncpu,
         ncpu ? cpus[0] : -1, ncpu ? cpus[ncpu - 1] : -1, n_exec, pin ? " (pinned)" : " (not pinned)",
         pin ? (pin_patcher ? "pinned" : "NOT pinned") : "not pinned", secs, (unsigned long long)patches);

    if (pin) {
        addf(out, cap, "  patcher CPU %d (%s); executor CPUs:", patcher_cpu, pin_patcher ? "pinned" : "NOT pinned");
        for (int i = 0; i < n_exec; i++) addf(out, cap, " %d(%s)", sh.ex[i].cpu, sh.ex[i].pin_ok ? "ok" : "NOT pinned");
        addf(out, cap, "\n  (CPU numbers only; this does not show core type or cache sharing)\n");
    }
    uint64_t calls = 0, na = 0, nb = 0, invalid = 0, old_before = 0, old_after = 0, moved = 0;
    int pin_fail = 0;
    for (int i = 0; i < n_exec; i++) {
        Exec *x = &sh.ex[i];
        calls += x->calls, na += x->na, nb += x->nb, invalid += x->invalid;
        old_before += x->old_before, old_after += x->old_after, moved += x->moved;
        if (pin && !x->pin_ok) pin_fail++;
        if (x->invalid)
            addf(out, cap, "  executor %d: first invalid value %ld\n", i, x->first_invalid);
        if (x->started && x->calls == 0 && !cancelled()) {
            addf(out, cap, "  executor %d never completed a call\n", i);
            fail = 1;
        }
    }
    if (invalid) fail = 1;
    if (pin && (pin_fail || !pin_patcher)) {
        unpinned = 1;
        addf(out, cap, "  pinning failed (%d executor(s)%s): not a valid cross-CPU run\n", pin_fail,
             pin_patcher ? "" : ", patcher");
    }

    if (sh.mode == M_CONCURRENT) {
        addf(out, cap, "  calls %llu: A %llu, B %llu, invalid %llu\n", (unsigned long long)calls,
             (unsigned long long)na, (unsigned long long)nb, (unsigned long long)invalid);
        if (!cancelled() && (!na || !nb)) {
            addf(out, cap, "  only one destination was ever seen\n");
            fail = 1;
        }
    } else if (sh.mode == M_VIS_A) {
        uint32_t *all = malloc(sizeof(uint32_t) * MAX_EPOCHS * MAX_EXEC);
        size_t n = 0;
        for (int i = 0; i < n_exec; i++)
            for (uint32_t k = 0; k < sh.ex[i].nlat; k++) all[n++] = sh.ex[i].lat_us[k];
        addf(out, cap, "  invalid %llu; not seen within 500 ms: %llu\n", (unsigned long long)invalid,
             (unsigned long long)misses);
        if (all && n) {
            qsort(all, n, sizeof *all, cmp_u32);
            addf(out, cap, "  observed %zu of %llu (patch x executor)\n", n, (unsigned long long)patches * (unsigned)n_exec);
            addf(out, cap, "  delay after hv_jit_finish returned (us): min %u, median %u, p99 %u, max %u\n", all[0],
                 all[n / 2], all[(n * 99) / 100], all[n - 1]);
        } else if (!cancelled()) {
            addf(out, cap, "  nothing observed\n");
            fail = 1;
        }
        free(all);
        addf(out, cap, "  (device measurement; includes scheduling and the loop; a call can run new code before the\n"
                       "   generation is published, so very short values mean 'visible at once')\n");
    } else {
        addf(out, cap, "  patches %llu; invalid %llu; no answer within 500 ms: %llu\n", (unsigned long long)patches,
             (unsigned long long)invalid, (unsigned long long)misses);
        addf(out, cap, "  old destination before ISB: %llu of %llu\n", (unsigned long long)old_before,
             (unsigned long long)(patches * (unsigned)n_exec));
        addf(out, cap, "  old destination after ISB:  %llu of %llu\n", (unsigned long long)old_after,
             (unsigned long long)(patches * (unsigned)n_exec));
        if (moved)
            addf(out, cap, "  excluded because the generation moved during the pair: %llu\n", (unsigned long long)moved);
        if (old_after)
            addf(out, cap, "  (observation, not a verdict: review before relying on this protocol in the engine)\n");
    }

    if (misses || timecap) {
        incomplete = 1;
        addf(out, cap, "  INCOMPLETE reasons: %s%s (a missed deadline, not proof that a thread is stuck)\n",
             misses ? "observation limit exceeded " : "", timecap ? "stage time cap (20 s) reached" : "");
    }

    mark(stage, "close region");
    hv_jit_region_close(&region);
    if (fail) return R_FAIL;
    if (cancelled()) return R_CANCELLED;
    if (incomplete) return R_INCOMPLETE;
    if (unpinned) return R_UNPINNED;
    return R_PASS;
}

/* Called from the UI thread before a run of stages (once, not per stage) and
   by the cancel button; neither blocks. */
JNIEXPORT void JNICALL Java_dev_muns_hengeprobe_MainActivity_beginMt(JNIEnv *env, jobject self) {
    (void)env;
    (void)self;
    atomic_store(&g_cancel, 0);
}

JNIEXPORT void JNICALL Java_dev_muns_hengeprobe_MainActivity_cancelMt(JNIEnv *env, jobject self) {
    (void)env;
    (void)self;
    atomic_store(&g_cancel, 1);
}

JNIEXPORT jstring JNICALL Java_dev_muns_hengeprobe_MainActivity_runMt(JNIEnv *env, jobject self, jint stage, jint seconds,
                                                                      jstring crash_path, jstring progress_path) {
    (void)self;
    char out[3072] = "";
    char err[768];

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

    int pipefd[2] = {-1, -1};
    int saved_err = -1;
    if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) == 0) {
        saved_err = dup(STDERR_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
    }

    static const char *const names[] = {"", "1 executor", "several executors", "visibility, no executor sync (A)",
                                        "visibility, ISB on executor (B)", "long run"};
    addf(out, sizeof out, "== stage %d: %s ==\n", (int)stage, stage >= 1 && stage <= 5 ? names[stage] : "?");
    int st = stage >= 1 && stage <= 5 ? run_stage((int)stage, seconds > 0 ? (int)seconds : 3, out, sizeof out) : R_FAIL;

    fflush(stderr);
    err[0] = 0;
    if (saved_err >= 0) {
        dup2(saved_err, STDERR_FILENO);
        close(saved_err);
        close(pipefd[1]);
        ssize_t r = read(pipefd[0], err, sizeof err - 1);
        err[r > 0 ? r : 0] = 0;
        close(pipefd[0]);
    }

    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);
    sigaction(SIGILL, &old_ill, NULL);

    if (g_crash_fd >= 0) close(g_crash_fd);
    g_crash_fd = -1;
    unlink(cpath);
    if (g_progress_fd >= 0) close(g_progress_fd);
    g_progress_fd = -1;
    (*env)->ReleaseStringUTFChars(env, crash_path, cpath);
    (*env)->ReleaseStringUTFChars(env, progress_path, ppath);

    if (err[0]) addf(out, sizeof out, "  diagnostics:\n%s", err);
    static const char *const state[] = {"PASS", "FAIL", "CANCELLED", "INCOMPLETE", "UNPINNED"};
    addf(out, sizeof out, "RESULT: %s (stage %d%s)\n", state[st], (int)stage,
         st == R_PASS && stage == 4 ? ", 관찰 시험 완료 - 동기화 정확성의 증명 아님" : "");
    __android_log_print(st == R_PASS ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, TAG, "%s", out);
    return (*env)->NewStringUTF(env, out);
}
