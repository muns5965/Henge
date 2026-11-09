#ifndef HENGEVM_PLATFORM_H
#define HENGEVM_PLATFORM_H

/* What the engine takes from the host OS beyond plain POSIX. On Apple platforms
   this is the Mach headers, unchanged. Elsewhere (Linux, Android) the few Mach
   calls the engine makes are given a small stand-in here, so the engine files
   themselves stay the same. */
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#else
#include <pthread.h>
#include <stdint.h>
#include <time.h>

/* Wait on a condition variable for a time from now (Apple's relative form).
   The wait is made absolute against the clock a plain condition variable uses. */
static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *cv, pthread_mutex_t *mu, const struct timespec *rel) {
    struct timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_sec += rel->tv_sec;
    abs.tv_nsec += rel->tv_nsec;
    if (abs.tv_nsec >= 1000000000L) {
        abs.tv_sec++;
        abs.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(cv, mu, &abs);
}

/* The guest's counter ticks at 24 MHz, the rate mach_absolute_time has on
   Apple silicon. Elsewhere it is made from the monotonic clock at the same rate. */
static inline uint64_t mach_absolute_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return ns * 3 / 125; /* 24,000,000 ticks per second */
}

/* CPU time of the calling thread in seconds (what the stats line wants from thread_info). */
static inline double hv_thread_cpu_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
#endif

#endif
