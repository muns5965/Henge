#define _GNU_SOURCE /* pipe2 */

/* Size ladder for the Linux/Android split JIT mapping, run inside an app
   process and returned as text for the screen, so no adb is needed.

   For each region size (the Java side steps 1, 16, 64, 256, 768 MiB):
   - map the region (memfd, RW view and RX view) and report where it landed;
   - write a mov/ret block at the start, the middle and the end of the region
     through the RW view and run each at its RX address; rewrite the first
     word of the start block and run it again (the 42 -> 7 check);
   - write one word into up to 256 pages spread across the whole region
     through the RW view and read each back through the RX view;
   - close the region before the next size, so memory does not pile up.

   Mapping a large region reserves address space; it does not commit it. Only
   the sampled pages are touched, so a pass means "mapped, ran code at three
   places, and sampled pages are coherent", not "that much memory was granted".

   Two aids for failures without adb:
   - A step marker is written to a progress file before every step. If the app
     is killed, the next launch shows the last step reached. That says where it
     got to, not why it ended.
   - A fatal-signal handler records signal, step and fault address in a crash
     file, shown on the next launch. jit_region.c's stderr diagnostics are
     captured through a pipe and appended to the result. */
#include <jni.h>

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "jit_region.h"

#define TAG "henge-jit"
#define PAGE 4096u
#define MAX_SAMPLE_PAGES 256u

typedef long (*jit_fn)(void);

static const uint32_t RET = 0xd65f03c0; /* ret */

static uint32_t mov_x0(unsigned imm) { /* mov x0, #imm (imm16) */
    return 0xd2800000u | (imm << 5);
}

static char g_step_buf[160];
static const char *volatile g_step = "idle";
static volatile int g_crash_fd = -1;
static int g_progress_fd = -1;

/* Async-signal-safe string building: no stdio in the handler. */
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

/* Name the step about to run, before it runs. */
static void mark(int mib, const char *step) {
    snprintf(g_step_buf, sizeof g_step_buf, "%d MiB: %s", mib, step);
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

/* Write mov x0,#imm; ret at word_index through the RW view and run it at the RX address. */
static int run_block(HvJitRegion *r, size_t word_index, unsigned imm, const char *label, int mib, char *out, size_t cap) {
    char step[64];
    uint32_t *code = r->base + word_index;
    uint32_t body[2] = {mov_x0(imm), RET};

    snprintf(step, sizeof step, "%s: write block", label);
    mark(mib, step);
    hv_jit_write(r, code, body, sizeof body);
    hv_jit_finish(r, code, 2);

    snprintf(step, sizeof step, "%s: run block", label);
    mark(mib, step);
    long got = ((jit_fn)(void *)code)();
    addf(out, cap, "  %s: returned %ld (expect %u)%s\n", label, got, imm, got == (long)imm ? "" : "  <-- MISMATCH");
    return got != (long)imm;
}

static int run_size(int mib, char *out, size_t cap) {
    HvJitRegion region;
    int bad = 0;

    mark(mib, "open region (memfd + RW and RX mmap)");
    if (hv_jit_region_open(&region, (size_t)mib << 20) != 0) {
        addf(out, cap, "  FAIL: no JIT region (errno %d: %s)\n", errno, strerror(errno));
        return 1;
    }
    addf(out, cap, "  mapped: rx=%p rw=%p\n", (void *)region.base, (void *)((char *)region.base + region.rw_diff));
    if (!region.rw_diff) {
        addf(out, cap, "  FAIL: no RW alias, so this is not the split shape\n");
        hv_jit_region_close(&region);
        return 1;
    }

    size_t cap_words = region.capacity;
    size_t mid = (cap_words / 2) & ~(size_t)1;
    size_t end = cap_words - 2;

    bad |= run_block(&region, 0, 42, "start", mib, out, cap);

    mark(mib, "start: rewrite one word through the RW alias");
    uint32_t seven = mov_x0(7);
    hv_jit_write(&region, region.base, &seven, sizeof seven);
    hv_jit_finish(&region, region.base, 1);
    mark(mib, "start: run the rewritten block");
    long got = ((jit_fn)(void *)region.base)();
    addf(out, cap, "  start after rewrite: returned %ld (expect 7)%s\n", got, got == 7 ? "" : "  <-- MISMATCH");
    bad |= got != 7;

    bad |= run_block(&region, mid, 55, "middle", mib, out, cap);
    bad |= run_block(&region, end, 99, "end", mib, out, cap);

    /* Sampled pages: one word in the middle of each, written through RW and read through RX. */
    size_t pages = (cap_words * sizeof(uint32_t)) / PAGE;
    size_t n = pages < MAX_SAMPLE_PAGES ? pages : MAX_SAMPLE_PAGES;
    size_t stride = pages / n;
    uint32_t *rw = (uint32_t *)((char *)region.base + region.rw_diff);
    volatile uint32_t *rx = region.base;

    mark(mib, "touch sample pages through the RW alias");
    for (size_t i = 0; i < n; i++) rw[i * stride * (PAGE / 4) + 512] = 0xA5A50000u | (uint32_t)(i & 0xffff);

    mark(mib, "read sample pages back through the RX view");
    size_t wrong = 0;
    for (size_t i = 0; i < n; i++)
        if (rx[i * stride * (PAGE / 4) + 512] != (0xA5A50000u | (uint32_t)(i & 0xffff))) wrong++;
    addf(out, cap, "  sampled %zu pages (of %zu), %zu read back wrong%s\n", n, pages, wrong, wrong ? "  <-- MISMATCH" : "");
    bad |= wrong != 0;

    mark(mib, "close region");
    hv_jit_region_close(&region);
    return bad;
}

JNIEXPORT jstring JNICALL Java_dev_muns_hengeprobe_MainActivity_runSize(JNIEnv *env, jobject self, jint mib, jstring crash_path,
                                                                        jstring progress_path) {
    (void)self;
    char out[1536] = "";
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

    /* Collect what jit_region.c writes to stderr while this size runs. */
    int pipefd[2] = {-1, -1};
    int saved_err = -1;
    if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) == 0) {
        saved_err = dup(STDERR_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
    }

    addf(out, sizeof out, "== %d MiB ==\n", (int)mib);
    int bad = run_size((int)mib, out, sizeof out);

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

    /* Finished this size without a fatal signal: no crash record to keep. The
       progress file stays until the Java side has run every size. */
    if (g_crash_fd >= 0) close(g_crash_fd);
    g_crash_fd = -1;
    unlink(cpath);
    if (g_progress_fd >= 0) close(g_progress_fd);
    g_progress_fd = -1;
    (*env)->ReleaseStringUTFChars(env, crash_path, cpath);
    (*env)->ReleaseStringUTFChars(env, progress_path, ppath);

    if (err[0]) addf(out, sizeof out, "  diagnostics:\n%s", err);
    addf(out, sizeof out, "RESULT: %s (%d MiB)\n", bad ? "FAIL" : "PASS", (int)mib);
    __android_log_print(bad ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, TAG, "%s", out);
    return (*env)->NewStringUTF(env, out);
}
