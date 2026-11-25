#ifndef HENGEVM_G7_DIAG_H
#define HENGEVM_G7_DIAG_H
/* Read-only host diagnostics for the G7 clone runner, written next to the g7-mem line (header-only so tests/test_g7_diag.c compiles the same code).
   Why it exists: the 900 s boot log could not tell a slow engine from a guest that is waiting. This adds what that log lacked, from procfs only:
     g7-diag: t=<s>s vm pswpin=<n>(+<d>) pswpout=<n>(+<d>) pgmajfault=<n>(+<d>) psi_some_avg10=<x> psi_some_us=<n>(+<d>) psi_full_avg10=<x> psi_full_us=<n>(+<d>) swaps=<dev>:<used>/<size>MiB,...
     g7-diag-thr: t=<s>s <tid>:<name> cpu_ms=<n>(+<d>) maj=<n>(+<d>) min=<n>(+<d>) | ...
   vm and psi are SYSTEM-WIDE (the whole phone); the per-thread part is this process only, so a vCPU thread's faults and CPU time are separate from the phone's.
   (+d) is the change since the previous line; the first line prints +?. A value that cannot be read (SELinux, an old kernel, a missing key) is printed as "?", never as 0.
   Numbers and short fixed words only: no path (a swap device is its base name, e.g. zram0). It reports; it changes nothing. */
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HV_G7D_UNKNOWN UINT64_MAX
#define HV_G7D_MAX_THREADS 24

/* "<key> <digits>" at the start of a line (/proc/vmstat); HV_G7D_UNKNOWN when absent. */
static inline uint64_t hv_g7d_vmstat_field(const char *text, const char *key) {
    size_t kl = strlen(key);
    const char *p = text ? text : "";
    while (*p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == ' ') {
            const char *q = p + kl + 1;
            if (*q < '0' || *q > '9') return HV_G7D_UNKNOWN;
            uint64_t v = 0;
            while (*q >= '0' && *q <= '9') { if (v > (UINT64_MAX - 9) / 10) return HV_G7D_UNKNOWN; v = v * 10 + (uint64_t)(*q - '0'); q++; }
            return v;
        }
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return HV_G7D_UNKNOWN;
}

/* /proc/pressure/memory: the line "some avg10=0.12 avg60=... total=123456" or "full ...". avg10 is copied as text (digits and one dot, at most cap-1 chars); *total is in microseconds.
   Returns 0 when the line, avg10 and total are all found; -1 otherwise (avg10 becomes "?", total unknown). */
static inline int hv_g7d_psi(const char *text, const char *kind, char *avg10, size_t cap, uint64_t *total) {
    size_t kl = strlen(kind);
    const char *p = text ? text : "";
    snprintf(avg10, cap, "?"); *total = HV_G7D_UNKNOWN;
    while (*p) {
        if (strncmp(p, kind, kl) == 0 && p[kl] == ' ') {
            const char *eol = strchr(p, '\n'); size_t len = eol ? (size_t)(eol - p) : strlen(p);
            char ln[200]; if (len >= sizeof ln) return -1;
            memcpy(ln, p, len); ln[len] = 0;
            const char *a = strstr(ln, "avg10="), *t = strstr(ln, "total=");
            if (!a || !t) return -1;
            a += 6; size_t i = 0;
            while (i + 1 < cap && ((*a >= '0' && *a <= '9') || *a == '.')) avg10[i++] = *a++;
            avg10[i] = 0;
            if (!i) { snprintf(avg10, cap, "?"); return -1; }
            t += 6; if (*t < '0' || *t > '9') return -1;
            uint64_t v = 0; while (*t >= '0' && *t <= '9') { if (v > (UINT64_MAX - 9) / 10) return -1; v = v * 10 + (uint64_t)(*t - '0'); t++; }
            *total = v; return 0;
        }
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return -1;
}

/* One /proc/self/task/<tid>/stat: "<tid> (<comm>) <state> <ppid> ... ". comm may contain spaces and ')' so the LAST ')' ends it.
   After it: state(3) ppid(4) pgrp(5) session(6) tty(7) tpgid(8) flags(9) minflt(10) cminflt(11) majflt(12) cmajflt(13) utime(14) stime(15). Times are clock ticks. */
typedef struct { uint64_t tid, minflt, majflt, utime, stime; char comm[20]; } HvG7DThr;
static inline int hv_g7d_parse_thread_stat(const char *text, HvG7DThr *t) {
    memset(t, 0, sizeof *t);
    if (!text) return -1;
    char *e; unsigned long long tid = strtoull(text, &e, 10);
    if (e == text) return -1;
    t->tid = tid;
    const char *lp = strchr(text, '('), *rp = strrchr(text, ')');
    if (!lp || !rp || rp < lp) return -1;
    size_t n = (size_t)(rp - lp - 1); if (n >= sizeof t->comm) n = sizeof t->comm - 1;
    for (size_t i = 0; i < n; i++) { unsigned char c = (unsigned char)lp[1 + i]; t->comm[i] = (c > 32 && c < 127 && c != '|') ? (char)c : '_'; }
    t->comm[n] = 0;
    const char *q = rp + 1; uint64_t f[13]; int idx = 0;           /* tokens 3..15 */
    while (idx < 13) {
        while (*q == ' ') q++;
        if (!*q) return -1;
        const char *tok = q; while (*q && *q != ' ' && *q != '\n') q++;
        if (idx == 0) f[idx] = 0;                                    /* state letter: not a number */
        else { char *ee; unsigned long long v = strtoull(tok, &ee, 10); if (ee == tok) return -1; f[idx] = v; }
        idx++;
    }
    t->minflt = f[7]; t->majflt = f[9]; t->utime = f[11]; t->stime = f[12];
    return 0;
}

/* /proc/swaps: "Filename Type Size Used Priority" in kB. Writes "zram0:1800/4096MiB" for up to two devices joined by ',', "none" with no device, "?" when unreadable (text NULL). */
static inline void hv_g7d_swaps(const char *text, char *out, size_t cap) {
    if (!text) { snprintf(out, cap, "?"); return; }
    out[0] = 0; int nd = 0; const char *p = strchr(text, '\n');   /* skip the header */
    if (p) p++;
    while (p && *p && nd < 2) {
        char name[64], type[16]; unsigned long long size, used;
        char ln[200]; const char *eol = strchr(p, '\n'); size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len >= sizeof ln) len = sizeof ln - 1;
        memcpy(ln, p, len); ln[len] = 0;
        if (sscanf(ln, "%63s %15s %llu %llu", name, type, &size, &used) == 4) {
            const char *b = strrchr(name, '/'); b = b ? b + 1 : name;
            char dev[24]; size_t i = 0; for (; b[i] && i < sizeof dev - 1; i++) dev[i] = (isalnum((unsigned char)b[i]) || b[i] == '_' || b[i] == '-') ? b[i] : '_';
            dev[i] = 0;
            size_t used_len = strlen(out);
            snprintf(out + used_len, cap - used_len, "%s%s:%llu/%lluMiB", nd ? "," : "", dev, used >> 10, size >> 10);
            nd++;
        }
        p = eol ? eol + 1 : NULL;
    }
    if (!nd) snprintf(out, cap, "none");
}

static inline int hv_g7d_read(const char *path, char *buf, size_t cap) {
    if (cap == 0) return -1;
    buf[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = 0;
    while (n + 1 < cap) { ssize_t r = read(fd, buf + n, cap - 1 - n); if (r <= 0) break; n += (size_t)r; }
    close(fd);
    buf[n] = 0;
    return (int)n;
}

/* "<n>(+<d>)" or "<n>(+?)" or "?": cur and prev may be unknown; a counter that went down prints +?. */
static inline int hv_g7d_delta(char *out, size_t cap, uint64_t cur, uint64_t prev) {
    if (cur == HV_G7D_UNKNOWN) return snprintf(out, cap, "?");
    if (prev == HV_G7D_UNKNOWN || cur < prev) return snprintf(out, cap, "%llu(+?)", (unsigned long long)cur);
    return snprintf(out, cap, "%llu(+%llu)", (unsigned long long)cur, (unsigned long long)(cur - prev));
}

typedef struct { uint64_t tid, cpu_ms, maj, min; } HvG7DThrPrev;
typedef struct {
    uint64_t pswpin, pswpout, pgmajfault, some_us, full_us;
    HvG7DThrPrev thr[HV_G7D_MAX_THREADS];
    int nthr;
} HvG7DPrev;

static inline void hv_g7d_prev_init(HvG7DPrev *pv) {
    memset(pv, 0, sizeof *pv);
    pv->pswpin = pv->pswpout = pv->pgmajfault = pv->some_us = pv->full_us = HV_G7D_UNKNOWN;
}

/* Builds the two lines (each ends in '\n'; the second is empty when no thread could be read) and remembers this sample as the next one's "previous". The caller writes them. */
static inline void hv_g7d_sample_lines(HvG7DPrev *pv, uint64_t t_s, char *l1, size_t c1, char *l2, size_t c2) {
    char vm[16384], psi[512], sw[2048];
    int have_vm = hv_g7d_read("/proc/vmstat", vm, sizeof vm) >= 0;
    int have_psi = hv_g7d_read("/proc/pressure/memory", psi, sizeof psi) >= 0;
    int have_sw = hv_g7d_read("/proc/swaps", sw, sizeof sw) >= 0;
    uint64_t pin = have_vm ? hv_g7d_vmstat_field(vm, "pswpin") : HV_G7D_UNKNOWN;
    uint64_t pout = have_vm ? hv_g7d_vmstat_field(vm, "pswpout") : HV_G7D_UNKNOWN;
    uint64_t pmaj = have_vm ? hv_g7d_vmstat_field(vm, "pgmajfault") : HV_G7D_UNKNOWN;
    char sa[16] = "?", fa[16] = "?"; uint64_t su = HV_G7D_UNKNOWN, fu = HV_G7D_UNKNOWN;
    if (have_psi) { hv_g7d_psi(psi, "some", sa, sizeof sa, &su); hv_g7d_psi(psi, "full", fa, sizeof fa, &fu); }
    char swd[80]; hv_g7d_swaps(have_sw ? sw : NULL, swd, sizeof swd);
    char a[48], b[48], c[48], d[48], e[48];
    hv_g7d_delta(a, sizeof a, pin, pv->pswpin); hv_g7d_delta(b, sizeof b, pout, pv->pswpout); hv_g7d_delta(c, sizeof c, pmaj, pv->pgmajfault);
    hv_g7d_delta(d, sizeof d, su, pv->some_us); hv_g7d_delta(e, sizeof e, fu, pv->full_us);
    snprintf(l1, c1, "g7-diag: t=%llus vm pswpin=%s pswpout=%s pgmajfault=%s psi_some_avg10=%s psi_some_us=%s psi_full_avg10=%s psi_full_us=%s swaps=%s\n", (unsigned long long)t_s, a, b, c, sa, d, fa, e, swd);
    pv->pswpin = pin; pv->pswpout = pout; pv->pgmajfault = pmaj; pv->some_us = su; pv->full_us = fu;

    l2[0] = 0;
    DIR *dir = opendir("/proc/self/task");
    if (!dir) { snprintf(l2, c2, "g7-diag-thr: t=%llus ?\n", (unsigned long long)t_s); return; }
    long hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100;
    size_t off = (size_t)snprintf(l2, c2, "g7-diag-thr: t=%llus", (unsigned long long)t_s);
    HvG7DThrPrev now[HV_G7D_MAX_THREADS]; int nn = 0;
    struct dirent *de;
    while ((de = readdir(dir)) && nn < HV_G7D_MAX_THREADS) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        char path[64], tx[600]; snprintf(path, sizeof path, "/proc/self/task/%.20s/stat", de->d_name);
        HvG7DThr t;
        if (hv_g7d_read(path, tx, sizeof tx) < 0 || hv_g7d_parse_thread_stat(tx, &t) != 0) continue;
        uint64_t cpu_ms = (t.utime + t.stime) * 1000ull / (uint64_t)hz;
        uint64_t pc = HV_G7D_UNKNOWN, pm = HV_G7D_UNKNOWN, pn = HV_G7D_UNKNOWN;
        for (int i = 0; i < pv->nthr; i++) if (pv->thr[i].tid == t.tid) { pc = pv->thr[i].cpu_ms; pm = pv->thr[i].maj; pn = pv->thr[i].min; }
        char x[48], y[48], z[48];
        hv_g7d_delta(x, sizeof x, cpu_ms, pc); hv_g7d_delta(y, sizeof y, t.majflt, pm); hv_g7d_delta(z, sizeof z, t.minflt, pn);
        if (off < c2) off += (size_t)snprintf(l2 + off, c2 - off, " %s%llu:%s cpu_ms=%s maj=%s min=%s", nn ? "| " : "", (unsigned long long)t.tid, t.comm, x, y, z);
        now[nn].tid = t.tid; now[nn].cpu_ms = cpu_ms; now[nn].maj = t.majflt; now[nn].min = t.minflt; nn++;
    }
    closedir(dir);
    if (off + 2 > c2) { l2[c2 - 2] = '\n'; l2[c2 - 1] = 0; } else { l2[off] = '\n'; l2[off + 1] = 0; }
    for (int i = 0; i < nn; i++) pv->thr[i] = now[i];
    pv->nthr = nn;
}
#endif
