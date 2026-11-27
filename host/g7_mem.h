#ifndef HENGEVM_G7_MEM_H
#define HENGEVM_G7_MEM_H
/* The G7 runner's memory line (compiled into host/g7_runner_real.c only; header-only so that tests/test_g7_mem.c can include the same code).
   Right after each `g7-alive:` line the watchdog writes ONE more line, built from two read-only procfs files:
     g7-mem: t=<s>s rss=<n>MiB peak=<n>MiB anon=<n>MiB file=<n>MiB swap=<n>MiB available=<n>MiB
   rss/peak/anon/file/swap come from /proc/self/status (VmRSS, VmHWM, RssAnon, RssFile, VmSwap: this process only) and available from /proc/meminfo (MemAvailable, Cached, Dirty, Writeback, SwapFree: the whole phone; Dirty and Writeback in MiB hide anything under 1 MiB).
   A value that the kernel does not give (or a file that cannot be read) is printed as "?", never as 0. Numbers and fixed words only: no path, no identifier. It reports; it changes nothing.
   It exists to tell, after a run that ended without a trace, whether the memory of the runner grew until the system ended it (peak and anon rising, available falling) or stayed flat. */
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define HV_G7M_UNKNOWN UINT64_MAX

typedef struct { uint64_t rss_kb, peak_kb, anon_kb, file_kb, shmem_kb, swap_kb, avail_kb, cached_kb, dirty_kb, writeback_kb, swapfree_kb; } HvG7Mem;

/* The value (in kB) of the line "<key>:   <digits> kB" in `text`; HV_G7M_UNKNOWN when there is no such line or it has no number. A key only matches at the start of a line. */
static inline uint64_t hv_g7m_field(const char *text, const char *key) {
    size_t kl = strlen(key);
    const char *p = text;
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == ':') {
            const char *q = p + kl + 1;
            while (*q == ' ' || *q == '\t') q++;
            if (*q < '0' || *q > '9') return HV_G7M_UNKNOWN;
            uint64_t v = 0;
            while (*q >= '0' && *q <= '9') { if (v > (UINT64_MAX - 9) / 10) return HV_G7M_UNKNOWN; v = v * 10 + (uint64_t)(*q - '0'); q++; }
            return v;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return HV_G7M_UNKNOWN;
}

/* Fills `m` from the two texts (either may be NULL or empty: its values are then unknown). */
static inline void hv_g7m_parse(HvG7Mem *m, const char *status, const char *meminfo) {
    if (!status) status = "";
    if (!meminfo) meminfo = "";
    m->rss_kb = hv_g7m_field(status, "VmRSS");
    m->peak_kb = hv_g7m_field(status, "VmHWM");
    m->anon_kb = hv_g7m_field(status, "RssAnon");
    m->file_kb = hv_g7m_field(status, "RssFile");
    m->shmem_kb = hv_g7m_field(status, "RssShmem");
    m->swap_kb = hv_g7m_field(status, "VmSwap");
    m->avail_kb = hv_g7m_field(meminfo, "MemAvailable");
    m->cached_kb = hv_g7m_field(meminfo, "Cached");
    m->dirty_kb = hv_g7m_field(meminfo, "Dirty");
    m->writeback_kb = hv_g7m_field(meminfo, "Writeback");
    m->swapfree_kb = hv_g7m_field(meminfo, "SwapFree");
}

/* Reads at most cap-1 bytes of a small procfs file into buf (always NUL-terminated); returns the byte count or -1. procfs files do not block on storage. */
static inline int hv_g7m_read(const char *path, char *buf, size_t cap) {
    if (cap == 0) return -1;
    buf[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = 0;
    while (n + 1 < cap) {
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    return (int)n;
}

static inline void hv_g7m_sample(HvG7Mem *m) {
    char st[8192], mi[8192];
    if (hv_g7m_read("/proc/self/status", st, sizeof st) < 0) st[0] = 0;
    if (hv_g7m_read("/proc/meminfo", mi, sizeof mi) < 0) mi[0] = 0;
    hv_g7m_parse(m, st, mi);
}

static inline int hv_g7m_mib(char *out, size_t cap, uint64_t kb) {
    if (kb == HV_G7M_UNKNOWN) return snprintf(out, cap, "?");
    return snprintf(out, cap, "%lluMiB", (unsigned long long)(kb >> 10));
}

/* The largest line hv_g7m_line can write, with every number at its 64-bit maximum (95 fixed bytes + 20 for t + 11 x 20 for the MiB values + NUL = 336): a caller buffer of this size never cuts the line. Real values are far smaller. */
#define HV_G7M_LINE_MAX 352
/* One line (ends in '\n'); written with one write() by the caller. */
static inline void hv_g7m_line(char *line, size_t cap, uint64_t t_s, const HvG7Mem *m) {
    char a[24], b[24], c[24], d[24], e[24], f[24], g[24], h[24], i[24], j[24], k[24];
    hv_g7m_mib(a, sizeof a, m->rss_kb); hv_g7m_mib(b, sizeof b, m->peak_kb); hv_g7m_mib(c, sizeof c, m->anon_kb);
    hv_g7m_mib(d, sizeof d, m->file_kb); hv_g7m_mib(e, sizeof e, m->swap_kb); hv_g7m_mib(f, sizeof f, m->avail_kb);
    hv_g7m_mib(k, sizeof k, m->shmem_kb); hv_g7m_mib(g, sizeof g, m->cached_kb); hv_g7m_mib(h, sizeof h, m->dirty_kb); hv_g7m_mib(i, sizeof i, m->writeback_kb); hv_g7m_mib(j, sizeof j, m->swapfree_kb);
    snprintf(line, cap, "g7-mem: t=%llus rss=%s peak=%s anon=%s file=%s shmem=%s swap=%s available=%s cached=%s dirty=%s writeback=%s swapfree=%s\n", (unsigned long long)t_s, a, b, c, d, k, e, f, g, h, i, j);
}
/* The line for one start-up stage (host/vm.c and core/machine.c call HV_STAGE(name) after each big allocation): the same process numbers, so the difference between two stages is what that stage cost. */
static inline void hv_g7m_stage_line(char *line, size_t cap, const char *name, const HvG7Mem *m) {
    char a[24], c[24], k[24], f[24];
    hv_g7m_mib(a, sizeof a, m->rss_kb); hv_g7m_mib(c, sizeof c, m->anon_kb); hv_g7m_mib(k, sizeof k, m->shmem_kb); hv_g7m_mib(f, sizeof f, m->avail_kb);
    snprintf(line, cap, "g7-mem-stage: %.40s rss=%s anon=%s shmem=%s available=%s\n", name, a, c, k, f);
}

/* /proc/self/smaps_rollup: the Pss line (shared pages divided between their mappings, so the JIT's two views of one memfd are not counted twice). Read rarely (the file is slower than status); ? when not readable. */
static inline uint64_t hv_g7m_pss_kb(void) {
    char b[2048];
    if (hv_g7m_read("/proc/self/smaps_rollup", b, sizeof b) < 0) return HV_G7M_UNKNOWN;
    return hv_g7m_field(b, "Pss");
}
static inline void hv_g7m_pss_line(char *line, size_t cap, uint64_t t_s, uint64_t pss_kb) {
    char a[24]; hv_g7m_mib(a, sizeof a, pss_kb);
    snprintf(line, cap, "g7-mem-pss: t=%llus pss=%s\n", (unsigned long long)t_s, a);
}
#endif
