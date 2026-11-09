/* The same two checks as tests/test_jit_linux.c, run inside an app process and
   returned as text for the screen, so no adb is needed to read the result.

   Two things beyond the plain test:
   - jit_region.c reports a failed mapping step on stderr; stderr is pointed at
     a pipe while the probe runs and the text is appended to the result.
   - If the probe takes a fatal signal (running the new code can), the handler
     records which step and signal into a file, and the next launch shows it. */
#define _GNU_SOURCE /* pipe2 */

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

typedef long (*jit_fn)(void);

static const uint32_t MOV_X0_42 = 0xd2800540; /* mov x0, #42 */
static const uint32_t MOV_X0_7 = 0xd28000e0;  /* mov x0, #7 */
static const uint32_t RET = 0xd65f03c0;       /* ret */

static const char *volatile g_step = "idle";
static volatile int g_crash_fd = -1;

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
    char buf[200];
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

static void addf(char *out, size_t cap, const char *fmt, ...) {
    size_t used = strlen(out);
    if (used >= cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + used, cap - used, fmt, ap);
    va_end(ap);
}

static int run_steps(char *out, size_t cap) {
    HvJitRegion region;
    int bad = 0;

    g_step = "open JIT region (memfd + RW and RX mmap)";
    if (hv_jit_region_open(&region, 1 << 20) != 0) {
        addf(out, cap, "FAIL: no JIT region (errno %d: %s)\n", errno, strerror(errno));
        return 1;
    }
    addf(out, cap, "region ok: rx=%p rw=%p\n", (void *)region.base, (void *)((char *)region.base + region.rw_diff));
    if (!region.rw_diff) {
        addf(out, cap, "FAIL: no RW alias, so this is not the split shape\n");
        hv_jit_region_close(&region);
        return 1;
    }

    g_step = "write block through the RW alias";
    uint32_t *code = hv_jit_reserve(&region, 2);
    uint32_t body[2] = {MOV_X0_42, RET};
    hv_jit_write(&region, code, body, sizeof body);
    hv_jit_finish(&region, code, 2);

    g_step = "run block at the RX address";
    long got = ((jit_fn)(void *)code)();
    addf(out, cap, "first run returned %ld (expect 42)\n", got);
    bad |= got != 42;

    g_step = "rewrite one word through the RW alias";
    hv_jit_write(&region, code, &MOV_X0_7, sizeof MOV_X0_7);
    hv_jit_finish(&region, code, 1);

    g_step = "run the rewritten block";
    got = ((jit_fn)(void *)code)();
    addf(out, cap, "after rewrite returned %ld (expect 7)\n", got);
    bad |= got != 7;

    g_step = "close";
    hv_jit_region_close(&region);
    return bad;
}

JNIEXPORT jstring JNICALL Java_dev_muns_hengeprobe_MainActivity_runProbe(JNIEnv *env, jobject self, jstring crash_path) {
    (void)self;
    char out[2048] = "";
    char err[1024];

    const char *path = (*env)->GetStringUTFChars(env, crash_path, NULL);
    g_crash_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

    struct sigaction sa, old_segv, old_bus, old_ill;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fatal;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);
    sigaction(SIGILL, &sa, &old_ill);

    /* Collect what jit_region.c writes to stderr while the probe runs. */
    int pipefd[2] = {-1, -1};
    int saved_err = -1;
    if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) == 0) {
        saved_err = dup(STDERR_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
    }

    int bad = run_steps(out, sizeof out);

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

    /* Finished without a fatal signal: nothing to report next launch. */
    if (g_crash_fd >= 0) close(g_crash_fd);
    g_crash_fd = -1;
    unlink(path);
    (*env)->ReleaseStringUTFChars(env, crash_path, path);

    if (err[0]) addf(out, sizeof out, "diagnostics:\n%s", err);
    addf(out, sizeof out, "%s\n", bad ? "RESULT: FAIL" : "RESULT: PASS");
    __android_log_print(bad ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, TAG, "%s", out);
    return (*env)->NewStringUTF(env, out);
}
