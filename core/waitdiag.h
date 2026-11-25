#ifndef HENGEVM_WAITDIAG_H
#define HENGEVM_WAITDIAG_H
/* DIAGNOSTIC ONLY: timing of the waits that can stall a boot. It changes no behaviour: every wait site is bracketed by hv_wd_begin()/hv_wd_end(), which only read the clock
   and update relaxed atomic counters (no lock, no allocation, nothing the guest can see). Header-only; the tables are weak definitions, so every translation unit that includes
   this header shares one copy (ELF and Mach-O both merge them).
   Why: a 900 s clone boot that stalled could not be explained from its log. For each site this keeps
     n          waits completed
     tot_ns     total time spent in them          max_ns   longest one ever          wmax_ns   longest since the last report (reset by the reader)
     inflight   threads inside the wait right now  since_ns host time the latest of them entered (a wait that never returns is invisible in tot/max; this is what shows it)
   and per vCPU, which site it is inside and since when. host/ui_runner_vm_clone.c prints them every 10 s (hv_wd_report). Sites that no thread ever entered are not printed. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dispatch.h"   /* machine.h, cpu.h, hv_diag_blocks */

enum {
    HV_WD_STW_LEAD,      /* stop_the_world(): the leader waiting until every other CPU is parked (the spin that follows hv_machine_kick_all) */
    HV_WD_STW_FLUSH,     /* stop_the_world(): the leader's hv_flush_blocks() itself (drops every translation) */
    HV_WD_STW_PARK,      /* stop_the_world(): another CPU parked on stw_cv until the leader finishes */
    HV_WD_UNPARK_STW,    /* unpark(): a CPU coming back from park, held on stw_cv because a stop-the-world is running */
    HV_WD_SYNC,          /* hv_cpu_sync(): waiting for another CPU to acknowledge a TLBI / branch-cache request */
    HV_WD_BIGLOCK,       /* hv_big_lock(): waiting for the machine-wide ticket lock (only when big_lock_on, only the contended path) */
    HV_WD_DEVLOCK,       /* hv_dev_lock(): the device mutex was busy (only the contended path) */
    HV_WD_CPU_OFF,       /* wait_while_off(): a CPU powered off in PSCI waiting for CPU_ON, no timeout */
    HV_WD_VBLK_IDLE,     /* virtio-blk worker waiting on cv for a request (idle is normal; also covers "held back by a fence") */
    HV_WD_VBLK_FENCE,    /* virtio-blk worker: a flush / Apple barrier waiting until every earlier request has finished (done_cv, no timeout) */
    HV_WD_VBLK_REQ,      /* virtio-blk: handle() of one read / write / other request that is not a fence, from claim to completion (host pread/pwrite time) */
    HV_WD_VBLK_FLUSH,    /* virtio-blk: handle() of a T_FLUSH or Apple-barrier request (ends in hv_disk_flush) */
    HV_WD_DISK_FSYNC,    /* hv_disk_flush(): the fsync / F_BARRIERFSYNC itself (not the HV_FLUSH=1 no-op) */
    HV_WD_DISK_READ,     /* hv_disk_read(): the pread loop */
    HV_WD_DISK_WRITE,    /* hv_disk_write(): the pwrite loop */
    HV_WD_N
};

typedef struct {
    uint64_t n, tot_ns, max_ns, wmax_ns, inflight, since_ns;
} HvWdSite;

#define HV_WD_CPUS HV_MAX_CPUS

/* Weak: one shared definition across translation units. */
__attribute__((weak)) HvWdSite hv_wd_site[HV_WD_N];
__attribute__((weak)) uint64_t hv_wd_cpu_since[HV_WD_CPUS];   /* host ns this vCPU entered the wait it is in; 0: not in one */
__attribute__((weak)) uint32_t hv_wd_cpu_site[HV_WD_CPUS];    /* which one (valid while hv_wd_cpu_since != 0) */

static inline const char *hv_wd_name(int s) {
    static const char *const nm[HV_WD_N] = {"stw_lead", "stw_flush", "stw_park", "unpark_stw", "cpu_sync", "big_lock", "dev_lock", "cpu_off",
                                            "vblk_idle", "vblk_fence", "vblk_req", "vblk_flush", "disk_fsync", "disk_read", "disk_write"};
    return s >= 0 && s < HV_WD_N ? nm[s] : "?";
}

static inline uint64_t hv_wd_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Entering a wait: returns the start time to hand to hv_wd_end(). `slot` is the vCPU index whose per-vCPU "in wait" marker to set, or -1 for none (device and disk threads).
   Only a vCPU's own thread writes its slot. hv_wd_begin_c / hv_wd_end_c take the slot from hv_self (core/ only: they name hv_self, which standalone tools that link
   host/disk.c without core/machine.c do not have; being static inline they are not emitted, so those tools still link). */
static inline uint64_t hv_wd_begin_s(int s, int slot) {
    uint64_t t0 = hv_wd_now();
    HvWdSite *w = &hv_wd_site[s];
    __atomic_store_n(&w->since_ns, t0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&w->inflight, 1, __ATOMIC_RELAXED);
    if (slot >= 0 && slot < HV_WD_CPUS) {
        __atomic_store_n(&hv_wd_cpu_site[slot], (uint32_t)s, __ATOMIC_RELAXED);
        __atomic_store_n(&hv_wd_cpu_since[slot], t0, __ATOMIC_RELAXED);
    }
    return t0;
}

static inline void hv_wd_end_s(int s, uint64_t t0, int slot) {
    uint64_t dt = hv_wd_now() - t0;
    HvWdSite *w = &hv_wd_site[s];
    if (slot >= 0 && slot < HV_WD_CPUS) __atomic_store_n(&hv_wd_cpu_since[slot], 0, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&w->inflight, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&w->n, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&w->tot_ns, dt, __ATOMIC_RELAXED);
    for (uint64_t m = __atomic_load_n(&w->max_ns, __ATOMIC_RELAXED); dt > m;)   /* rare after warm-up */
        if (__atomic_compare_exchange_n(&w->max_ns, &m, dt, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) break;
    for (uint64_t m = __atomic_load_n(&w->wmax_ns, __ATOMIC_RELAXED); dt > m;)
        if (__atomic_compare_exchange_n(&w->wmax_ns, &m, dt, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) break;
}

static inline uint64_t hv_wd_begin(int s) { return hv_wd_begin_s(s, -1); }
static inline void hv_wd_end(int s, uint64_t t0) { hv_wd_end_s(s, t0, -1); }
static inline int hv_wd_self_slot(void) { HvCpu *c = hv_self; return c ? (int)c->index : -1; }
static inline uint64_t hv_wd_begin_c(int s) { return hv_wd_begin_s(s, hv_wd_self_slot()); }
static inline void hv_wd_end_c(int s, uint64_t t0) { hv_wd_end_s(s, t0, hv_wd_self_slot()); }

/* One line per site that was ever entered, then one per vCPU, appended to out (cap bytes in all, always NUL-terminated, cut off rather than overrun).
   Reader only: it resets wmax_ns (the "since the last report" maximum), nothing else. `t_s` is the runner's seconds since start, for the line prefix.
   Line formats:
     wd: t=<s>s <site> n=<n>(+<d>) tot_ms=<ms>(+<d>) max_ms=<ms> wmax_ms=<ms> in=<threads inside now> age_ms=<how long ago the latest of them entered>
     wd-cpu: t=<s>s cpu<i> blocks=<n> pc=<hex> el=<n> exit=<HV_EXIT_*> off=<0/1> halted=<0/1> active=<0/1> tlb_live=<0/1> req=<req_seq>/<ack_seq> in=<site|-> age_ms=<n>
   pc / el / exit are what the dispatcher last stored for that CPU (they move at dispatcher entries, not inside a running block): a CPU stuck in one block or in a wait keeps its old values,
   which is the point. They are read from another thread without a lock (relaxed loads of plain fields: a torn or stale value is possible and harmless for a diagnostic). */
static inline size_t hv_wd_report(char *out, size_t cap, uint64_t t_s, HvMachine *m) {
    static uint64_t prev_n[HV_WD_N], prev_tot[HV_WD_N];   /* only the runner's main thread calls this */
    size_t len = 0;
    if (!out || !cap) return 0;
    out[0] = 0;
    uint64_t now = hv_wd_now();
#define HV_WD_APPEND(...) do { if (len + 1 < cap) { int k_ = snprintf(out + len, cap - len, __VA_ARGS__); if (k_ > 0) len = len + (size_t)k_ < cap ? len + (size_t)k_ : cap - 1; } } while (0)
    for (int s = 0; s < HV_WD_N; s++) {
        HvWdSite *w = &hv_wd_site[s];
        uint64_t n = __atomic_load_n(&w->n, __ATOMIC_RELAXED), tot = __atomic_load_n(&w->tot_ns, __ATOMIC_RELAXED);
        uint64_t in = __atomic_load_n(&w->inflight, __ATOMIC_RELAXED), since = __atomic_load_n(&w->since_ns, __ATOMIC_RELAXED);
        uint64_t wmax = __atomic_exchange_n(&w->wmax_ns, 0, __ATOMIC_RELAXED), max = __atomic_load_n(&w->max_ns, __ATOMIC_RELAXED);
        if (!n && !in) continue;
        HV_WD_APPEND("wd: t=%llus %s n=%llu(+%llu) tot_ms=%llu(+%llu) max_ms=%.3f wmax_ms=%.3f in=%llu age_ms=%llu\n", (unsigned long long)t_s, hv_wd_name(s),
                     (unsigned long long)n, (unsigned long long)(n - prev_n[s]), (unsigned long long)(tot / 1000000), (unsigned long long)((tot - prev_tot[s]) / 1000000),
                     max / 1e6, wmax / 1e6, (unsigned long long)in, (unsigned long long)(in && since && now > since ? (now - since) / 1000000 : 0));
        prev_n[s] = n, prev_tot[s] = tot;
    }
    for (int i = 0; m && i < m->ncpus && i < HV_WD_CPUS; i++) {
        HvCpu *c = m->cpus[i];
        if (!c) continue;
        HvPerCpu *p = c->pc_state;
        uint64_t since = __atomic_load_n(&hv_wd_cpu_since[i], __ATOMIC_RELAXED);
        uint32_t site = __atomic_load_n(&hv_wd_cpu_site[i], __ATOMIC_RELAXED);
        HV_WD_APPEND("wd-cpu: t=%llus cpu%d blocks=%llu pc=%llx el=%u exit=%u off=%d halted=%d active=%d tlb_live=%d req=%llu/%llu in=%s age_ms=%llu\n", (unsigned long long)t_s, i,
                     (unsigned long long)hv_diag_blocks((unsigned)i), (unsigned long long)__atomic_load_n(&c->pc, __ATOMIC_RELAXED),
                     (unsigned)__atomic_load_n(&c->el, __ATOMIC_RELAXED), (unsigned)__atomic_load_n(&c->exit_reason, __ATOMIC_RELAXED),
                     (int)__atomic_load_n(&c->off, __ATOMIC_RELAXED), (int)__atomic_load_n(&c->halted, __ATOMIC_RELAXED),
                     p ? __atomic_load_n(&p->active, __ATOMIC_RELAXED) : -1, p ? __atomic_load_n(&p->tlb_live, __ATOMIC_RELAXED) : -1,
                     (unsigned long long)(p ? __atomic_load_n(&p->req_seq, __ATOMIC_RELAXED) : 0), (unsigned long long)(p ? __atomic_load_n(&p->ack_seq, __ATOMIC_RELAXED) : 0),
                     since ? hv_wd_name((int)site) : "-", (unsigned long long)(since && now > since ? (now - since) / 1000000 : 0));
    }
#undef HV_WD_APPEND
    return len;
}

#endif
