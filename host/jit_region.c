#define _GNU_SOURCE /* memfd_create on Linux and Android */

#include "jit_region.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#else
#include <errno.h>
#include <stdio.h>
#if defined(__ANDROID__)
#include <android/log.h>
#endif
#define TARGET_OS_OSX 0
#endif

#if !defined(__APPLE__)
/* Said only when a mapping step fails, so a normal run prints nothing. stderr
   shows it in an adb shell and in CI; on Android it also goes to logcat, which
   is where an app process's output ends up. err is 0 for a failure that did not
   come from a system call, so no stale errno is printed. */
static void jit_diag(const char *step, int err) {
    char line[160];
    if (err) snprintf(line, sizeof line, "jit_region: %s failed: %s (errno %d)", step, strerror(err), err);
    else snprintf(line, sizeof line, "jit_region: %s failed", step);
    fprintf(stderr, "%s\n", line);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_ERROR, "henge-jit", "%s", line);
#endif
}
#endif

#if defined(__APPLE__)
/* The split shape on a host that can make it itself: plain RW memory, an
   alias, and the original turned R-X. Works for macOS binaries without the
   hardened runtime; on iOS the executable half has to come from a debugger. */
static int open_split(HvJitRegion *region, size_t bytes) {
    void *rx = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (rx == MAP_FAILED) return -1;
    vm_address_t rw = 0;
    vm_prot_t cur, max;
    if (vm_remap(mach_task_self(), &rw, bytes, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (vm_address_t)rx, FALSE, &cur, &max,
                 VM_INHERIT_NONE) != KERN_SUCCESS ||
        mprotect(rx, bytes, PROT_READ | PROT_EXEC) != 0) {
        munmap(rx, bytes);
        return -1;
    }
    if (hv_jit_region_attach(region, rx, (void *)rw, bytes) != 0) return -1;
    region->owned = 1;
    return 0;
}
#else
/* Linux and Android: one memfd, mapped twice. The RW view writes code and the
   RX view runs it; both name the same pages, so a write through the alias is
   what the executable address executes. Whether the kernel and SELinux allow
   an executable view of a memfd is exactly what the device test checks. */
static int open_split(HvJitRegion *region, size_t bytes) {
    int err;
    int fd = memfd_create("henge-jit", 0);
    if (fd < 0) {
        err = errno;
        jit_diag("memfd_create", err);
        errno = err;
        return -1;
    }
    if (ftruncate(fd, (off_t)bytes) != 0) {
        err = errno;
        jit_diag("ftruncate", err);
        close(fd);
        errno = err;
        return -1;
    }
    void *rw = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (rw == MAP_FAILED) {
        err = errno;
        jit_diag("RW mmap", err);
        close(fd);
        errno = err;
        return -1;
    }
    void *rx = mmap(NULL, bytes, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    if (rx == MAP_FAILED) {
        err = errno;
        jit_diag("RX mmap", err);
        munmap(rw, bytes);
        close(fd);
        errno = err;
        return -1;
    }
    close(fd);
    if (hv_jit_region_attach(region, rx, rw, bytes) != 0) {
        jit_diag("region attach (bad address or size)", 0);
        munmap(rw, bytes);
        munmap(rx, bytes);
        errno = EINVAL;
        return -1;
    }
    region->owned = 1;
    return 0;
}
#endif

int hv_jit_region_open(HvJitRegion *region, size_t bytes) {
    memset(region, 0, sizeof *region);
    if (getenv("HV_JIT_SPLIT")) return open_split(region, bytes);
#if TARGET_OS_OSX
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) return -1;
    region->base = p;
    region->capacity = bytes / sizeof(uint32_t);
    region->owned = 1;
    return 0;
#else
    /* Not macOS. iOS: works only where nothing enforces code signing (tests on a
       jailbroken device). Linux and Android: the memfd split shape is the only one. */
    return open_split(region, bytes);
#endif
}

int hv_jit_region_attach(HvJitRegion *region, void *rx, void *rw, size_t bytes) {
    memset(region, 0, sizeof *region);
    if (!rx || !rw || bytes < 4096) return -1;
    region->base = rx;
    region->rw_diff = (intptr_t)((uint8_t *)rw - (uint8_t *)rx);
    region->capacity = bytes / sizeof(uint32_t);
    return 0;
}

void hv_jit_region_close(HvJitRegion *region) {
    if (region->owned && region->base) {
        size_t bytes = region->capacity * sizeof(uint32_t);
        if (region->rw_diff) munmap((uint8_t *)region->base + region->rw_diff, bytes);
        munmap(region->base, bytes);
    }
    memset(region, 0, sizeof *region);
}

void hv_jit_write(HvJitRegion *region, uint32_t *start, const void *src, size_t bytes) {
    /* One instruction may be rewritten while other CPUs run it (chaining a branch).
       memcpy may copy a word in pieces, and a CPU fetching in between takes half of
       the old branch and half of the new one: a jump to nowhere. */
    if (bytes == 4) {
        uint32_t word;
        memcpy(&word, src, 4);
#if TARGET_OS_OSX
        if (!region->rw_diff) pthread_jit_write_protect_np(0);
#endif
        __atomic_store_n((uint32_t *)((uint8_t *)start + region->rw_diff), word, __ATOMIC_RELEASE);
        return;
    }
    if (region->rw_diff) {
        memcpy((uint8_t *)start + region->rw_diff, src, bytes);
        return;
    }
#if TARGET_OS_OSX
    pthread_jit_write_protect_np(0); /* MAP_JIT: writable on this thread only, until hv_jit_finish */
#endif
    memcpy(start, src, bytes);
}

void hv_jit_finish(HvJitRegion *region, uint32_t *start, size_t words) {
    (void)region;
#if TARGET_OS_OSX
    if (!region->rw_diff) pthread_jit_write_protect_np(1);
#endif
    if (!start || !words) return;
#if defined(__APPLE__)
    sys_icache_invalidate(start, words * sizeof(uint32_t));
#else
    __builtin___clear_cache((char *)start, (char *)(start + words));
#endif
}

/* Any thread: an even number of words (blocks stay 8-byte aligned), taken atomically. */
uint32_t *hv_jit_reserve(HvJitRegion *region, size_t words) {
    words = (words + 1) & ~(size_t)1;
    size_t at = __atomic_fetch_add(&region->used, words, __ATOMIC_RELAXED);
    size_t cap = region->limit && region->limit < region->capacity ? region->limit : region->capacity;
    if (at + words > cap) return NULL; /* full: the flush resets used */
    return region->base + at;
}

void hv_jit_reset(HvJitRegion *region) {
    region->used = 0;
}
