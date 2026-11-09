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
#include <stdint.h>
#include <time.h>

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
