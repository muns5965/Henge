#ifndef HENGEVM_G7_NVRAM_H
#define HENGEVM_G7_NVRAM_H
/* NVRAM diagnostics (S0, default ON) and an opt-in pre-seed (S1, default OFF) for the G7 clone runner. Header-only so tests/test_g7_nvram.c compiles the same code (Linux, no guest).

   How NVRAM works here [V code]: there is NO NVRAM device in this host. The vmapple guest keeps its NVRAM inside the AUXILIARY STORAGE image (macos_aux.img, the first 16 KiB are hidden metadata,
   host/disk.c offset). iBoot reaches it through the bdif backdoor (devices/bdif.c, aux channel) and the kernel through the aux virtio-blk (devices/virtio_blk.c); both end in hv_disk_read /
   hv_disk_write / hv_disk_flush on the same HvDisk. So a guest "NVRAM write" is an ordinary aux sector write that the host accepts (pwrite on the clone's writable aux file) and a "sync" is a flush.
   The host does not parse, filter or keep any variable.
   What this file adds:
   S0  hv_disk_set_trace observer on the aux disk (read-only: it looks at the bytes, never changes a request or its result). It logs each aux write (offset, length, first 32 bytes), each flush, and
       any aux read/write whose bytes contain Apple NVRAM "v3" entries (variable name, GUID, state, value size, first 32 bytes of the value). The v3 layout used for recognising them comes from the public
       apple-nvram crate documentation (store signature "3VVN", 24-byte store header, 36-byte entry header 0x55AA, CRC-32 of the value); that this is the layout the vmapple guest uses is [I], not verified.
       Lines are "g7-nvram: ...", at most G7NV_LINE_CAP, plus one summary line at stop (outside the cap).
   S1  g7nv_preseed: only when asked (marker file or environment), find a VALID "3VVN" store in the aux image, and append the requested variables there (old copies of the same name/GUID are marked
       deleted, as the public crate does). If no valid store is found NOTHING is written. boot-volume is never made up: it is written only from a string the person gives.
   Not known [N]: where in aux the store lives, the value encoding the guest expects for auto-boot ("true" with or without a NUL), and the boot-volume format. */
#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "disk.h"

/* integ7b T4: the disk operations the pre-seed uses, as macros only so a host test can inject faults (short write, failing write/flush, no free space). The shipped build never defines them. */
#ifndef G7NV_DISK_WRITE
#define G7NV_DISK_WRITE(d, off, buf, len) hv_disk_write((d), (off), (buf), (len))
#endif
#ifndef G7NV_DISK_FLUSH
#define G7NV_DISK_FLUSH(d) hv_disk_flush((d))
#endif
#ifndef G7NV_FREE_BYTES
static inline uint64_t g7nv_free_bytes_default(const char *path) { char dir[1024]; snprintf(dir, sizeof dir, "%s", path); char *sl = strrchr(dir, '/'); if (sl) *sl = 0; else strcpy(dir, "."); struct statvfs v; return statvfs(dir, &v) == 0 ? (uint64_t)v.f_bavail * v.f_frsize : 0; }
#define G7NV_FREE_BYTES(path) g7nv_free_bytes_default(path)
#endif

#define G7NV_LINE_CAP 200          /* all "g7-nvram:" lines but the summary and the pre-seed lines */
#define G7NV_OP_LINES 100          /* of those, at most this many for plain aux ops (write/flush/first reads); the rest is for NVRAM entries */
#define G7NV_SCAN_MAX (1u << 20)   /* bytes of one request that are searched for entries */
#define G7NV_SEEN_MAX 128
#define G7NV_NKEYS 5
#define G7NV_PART 0x10000u         /* v3 store partition size in the public crate */

static const uint8_t g7nv_guid_common[16] = {0x7C, 0x43, 0x61, 0x10, 0xAB, 0x2A, 0x4B, 0xBB, 0xA8, 0x80, 0xFE, 0x41, 0x99, 0x5C, 0x9F, 0x82};   /* 7C436110-AB2A-4BBB-A880-FE41995C9F82 */
static const uint8_t g7nv_guid_system[16] = {0x40, 0xA0, 0xDD, 0xD2, 0x77, 0xF8, 0x43, 0x92, 0xB4, 0xA3, 0x1E, 0x73, 0x04, 0x20, 0x65, 0x16}; /* 40A0DDD2-77F8-4392-B4A3-1E7304206516 */
static const char *const g7nv_key_names[G7NV_NKEYS] = {"auto-boot", "boot-volume", "boot-args", "boot-command", "one-time-boot-command"};
/* Names that XNU's IONVRAM routes to the Apple SYSTEM GUID (public xnu source, gNVRAMSystemList); anything else goes to the common GUID [I]. */
static const char *const g7nv_system_names[] = {"allow-root-hash-mismatch", "auto-boot", "auto-boot-halt-stage", "base-system-path", "boot-args", "boot-command", "boot-image", "bootdelay",
    "com.apple.System.boot-nonce", "darkboot", "emu", "one-time-boot-command", "policy-nonce-digests", "prevent-restores", "prev-lang:kbd", "root-live-fs", "SystemAudioVolume",
    "SystemAudioVolumeExtension", "SystemAudioVolumeSaved", NULL};

static inline uint32_t g7nv_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void g7nv_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* CRC-32 (IEEE, as crc32fast); crc32("123456789") = 0xCBF43926. */
static inline uint32_t g7nv_crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}

static inline const char *g7nv_guid_name(const uint8_t *g, char *tmp, size_t cap) {
    if (!memcmp(g, g7nv_guid_system, 16)) return "system";
    if (!memcmp(g, g7nv_guid_common, 16)) return "common";
    size_t k = 0;
    for (int i = 0; i < 16 && k + 3 < cap; i++) k += (size_t)snprintf(tmp + k, cap - k, "%02x", g[i]);
    return tmp;
}
static inline int g7nv_known_guid(const uint8_t *g) { return !memcmp(g, g7nv_guid_system, 16) || !memcmp(g, g7nv_guid_common, 16); }
static inline int g7nv_is_system_name(const char *n) { for (int i = 0; g7nv_system_names[i]; i++) if (!strcmp(n, g7nv_system_names[i])) return 1; return 0; }

/* One variable entry candidate at b+pos: 36-byte header {u16 0x55AA, u8 state, u8 0, u32 attrs, u32 name_size (with NUL), u32 data_size, guid[16], u32 crc(value)}, name, NUL, value. */
typedef struct { size_t pos, total; uint8_t state; uint32_t attrs, name_size, data_size, crc; const uint8_t *guid, *val; size_t val_avail; int complete, crc_ok; char name[64]; } G7NvEnt;

static inline int g7nv_state_ok(uint8_t s) { return s == 0x7F || s == 0x7C || s == 0xFD || s == 0xFE; }

static inline int g7nv_parse_entry(const uint8_t *b, size_t len, size_t pos, G7NvEnt *e) {
    if (pos > len || len - pos < 36) return 0;
    const uint8_t *h = b + pos;
    if (h[0] != 0xAA || h[1] != 0x55 || !g7nv_state_ok(h[2]) || h[3] != 0) return 0;
    uint32_t ns = g7nv_le32(h + 8), ds = g7nv_le32(h + 12);
    if (ns < 2 || ns > 64 || ds > G7NV_PART) return 0;
    if (len - pos - 36 < ns) return 0;                       /* the name must be in view */
    const uint8_t *nm = h + 36;
    for (uint32_t i = 0; i + 1 < ns; i++) if (nm[i] < 0x21 || nm[i] > 0x7E) return 0;
    if (nm[ns - 1] != 0) return 0;
    memset(e, 0, sizeof *e);
    memcpy(e->name, nm, ns);
    e->pos = pos; e->state = h[2]; e->attrs = g7nv_le32(h + 4); e->name_size = ns; e->data_size = ds; e->guid = h + 16; e->crc = g7nv_le32(h + 32);
    e->total = 36 + (size_t)ns + ds;
    e->val = nm + ns;
    size_t after = len - pos - 36 - ns;
    e->val_avail = after < ds ? after : ds;
    e->complete = after >= ds;
    if (e->complete) { e->crc_ok = g7nv_crc32(e->val, ds) == e->crc; if (!e->crc_ok) return 0; }
    else if (!g7nv_known_guid(e->guid)) return 0;            /* cut-off entry: only trusted with one of the two Apple GUIDs */
    return 1;
}

typedef struct {
    pthread_mutex_t mu;
    FILE *out;
    int on, summarized;
    uint64_t t0_ms;
    unsigned op_lines, hit_lines, suppressed, repeats, plain_reads_logged;
    uint64_t seq, reads, writes, flushes, rbytes, wbytes, rfail, wfail, hdrs, sets, dels, gets;
    uint32_t seen[G7NV_SEEN_MAX]; unsigned nseen;
    struct { int seen; unsigned n_set, n_del, n_get; char op[8]; uint8_t state; uint32_t dsz; char guid[40]; char hex[40]; char txt[24]; } key[G7NV_NKEYS];
} G7Nv;

static inline uint64_t g7nv_now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u; }

static inline void g7nv_init(G7Nv *n, FILE *out, int on) {
    memset(n, 0, sizeof *n);
    pthread_mutex_init(&n->mu, NULL);
    n->out = out; n->on = on; n->t0_ms = g7nv_now_ms();
}

/* 32 bytes at most: hex string and a text view (non-printable bytes as '.'). */
static inline void g7nv_hex32(const uint8_t *p, size_t n, char *hex, size_t hcap, char *txt, size_t tcap) {
    if (n > 32) n = 32;
    size_t k = 0;
    for (size_t i = 0; i < n && k + 3 <= hcap; i++) k += (size_t)snprintf(hex + k, hcap - k, "%02x", p[i]);
    if (hcap) hex[k < hcap ? k : hcap - 1] = 0;
    size_t t = 0;
    for (size_t i = 0; i < n && t + 1 < tcap; i++) txt[t++] = p[i] >= 0x20 && p[i] < 0x7F && p[i] != '"' ? (char)p[i] : '.';
    if (tcap) txt[t] = 0;
}

static inline uint32_t g7nv_fnv(uint32_t h, const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; } return h; }

static inline void g7nv_vline(G7Nv *n, int is_hit, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static inline void g7nv_vline(G7Nv *n, int is_hit, const char *fmt, ...) {
    unsigned *cnt = is_hit ? &n->hit_lines : &n->op_lines;
    unsigned lim = is_hit ? G7NV_LINE_CAP - G7NV_OP_LINES : G7NV_OP_LINES;
    if (*cnt >= lim) { n->suppressed++; return; }
    (*cnt)++;
    char buf[640];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    fprintf(n->out, "g7-nvram: n=%llu t=%llums %s\n", (unsigned long long)n->seq, (unsigned long long)(g7nv_now_ms() - n->t0_ms), buf);
    fflush(n->out);
}

/* Entries and store headers inside one request's bytes; logs (deduplicated) and tracks the key variables. */
static inline void g7nv_scan(G7Nv *n, int op, uint64_t off, const uint8_t *b, size_t len, int *any) {
    size_t lim = len > G7NV_SCAN_MAX ? G7NV_SCAN_MAX : len;
    for (size_t i = 0; i < lim;) {
        if (b[i] == '3' && i + 24 <= lim && !memcmp(b + i, "3VVN", 4) && b[i + 14] == 1) {
            n->hdrs++; *any = 1;
            g7nv_vline(n, 1, "%s store-header aux_off=0x%llx size=%u generation=%u state=0x%02x flags=0x%02x system_size=%u common_size=%u [I: v3 \"3VVN\" layout]",
                       op == HV_DISK_TRACE_WRITE ? "W" : "R", (unsigned long long)(off + i), g7nv_le32(b + i + 4), g7nv_le32(b + i + 8), b[i + 12], b[i + 13], g7nv_le32(b + i + 16), g7nv_le32(b + i + 20));
            i += 24; continue;
        }
        G7NvEnt e;
        if (b[i] == 0xAA && g7nv_parse_entry(b, lim, i, &e)) {
            const char *what = op == HV_DISK_TRACE_WRITE ? (e.state == 0x7F ? "set" : "delete") : "get";
            if (op == HV_DISK_TRACE_WRITE) { if (e.state == 0x7F) n->sets++; else n->dels++; } else n->gets++;
            *any = 1;
            char gtmp[40], hex[80], txt[40];
            const char *gn = g7nv_guid_name(e.guid, gtmp, sizeof gtmp);
            g7nv_hex32(e.val, e.val_avail, hex, sizeof hex, txt, sizeof txt);
            for (int k = 0; k < G7NV_NKEYS; k++) if (!strcmp(e.name, g7nv_key_names[k])) {
                n->key[k].seen = 1; { unsigned *c = e.state == 0x7F && op == HV_DISK_TRACE_WRITE ? &n->key[k].n_set : op == HV_DISK_TRACE_WRITE ? &n->key[k].n_del : &n->key[k].n_get; (*c)++; }
                snprintf(n->key[k].op, sizeof n->key[k].op, "%s", what); n->key[k].state = e.state; n->key[k].dsz = e.data_size;
                snprintf(n->key[k].guid, sizeof n->key[k].guid, "%s", gn); snprintf(n->key[k].hex, sizeof n->key[k].hex, "%.32s", hex); snprintf(n->key[k].txt, sizeof n->key[k].txt, "%.20s", txt);
            }
            uint32_t h = 2166136261u;
            h = g7nv_fnv(h, what, strlen(what)); h = g7nv_fnv(h, e.name, strlen(e.name)); h = g7nv_fnv(h, e.guid, 16); h = g7nv_fnv(h, &e.state, 1); h = g7nv_fnv(h, &e.crc, 4); h = g7nv_fnv(h, &e.data_size, 4);
            int dup = 0;
            for (unsigned s = 0; s < n->nseen; s++) if (n->seen[s] == h) { dup = 1; break; }
            if (dup) n->repeats++;
            else {
                if (n->nseen < G7NV_SEEN_MAX) n->seen[n->nseen++] = h;
                g7nv_vline(n, 1, "%s %s name=%s guid=%s state=0x%02x size=%u val32=%s txt=\"%s\"%s aux_off=0x%llx [I: heuristic]", op == HV_DISK_TRACE_WRITE ? "W" : "R", what, e.name, gn, e.state, e.data_size, hex, txt,
                           e.complete ? "" : " (cut off by the request end)", (unsigned long long)(off + i));
            }
            i += e.complete ? e.total : 1;
            continue;
        }
        i++;
    }
}

/* The hv_disk_set_trace callback for the AUX disk. ctx is a G7Nv. */
static inline void g7nv_trace(void *ctx, int op, uint64_t off, const void *buf, uint64_t len, int rc) {
    G7Nv *n = ctx;
    if (!n || !n->on) return;
    pthread_mutex_lock(&n->mu);
    n->seq++;
    char hex[80], txt[40];
    if (op == HV_DISK_TRACE_FLUSH) {
        n->flushes++;
        g7nv_vline(n, 0, "F aux flush rc=%d", rc);
    } else if (buf) {
        int any = 0;
        if (op == HV_DISK_TRACE_WRITE) { n->writes++; n->wbytes += len; if (rc) n->wfail++; }
        else { n->reads++; if (rc == 0) n->rbytes += len; else n->rfail++; }
        if (op == HV_DISK_TRACE_WRITE || rc == 0) {
            size_t hl = len < 32 ? (size_t)len : 32;
            g7nv_hex32(buf, hl, hex, sizeof hex, txt, sizeof txt);
            if (op == HV_DISK_TRACE_WRITE) g7nv_vline(n, 0, "W aux_off=0x%llx len=%llu rc=%d head32=%s", (unsigned long long)off, (unsigned long long)len, rc, hex);
            else if (n->plain_reads_logged < 8) { n->plain_reads_logged++; g7nv_vline(n, 0, "R aux_off=0x%llx len=%llu head32=%s (first reads only; later plain reads are counted)", (unsigned long long)off, (unsigned long long)len, hex); }
            g7nv_scan(n, op, off, buf, (size_t)len, &any);
        }
    }
    pthread_mutex_unlock(&n->mu);
}

/* One summary line, once. Counts what the guest asked of the aux disk; recognition of entries is heuristic, so "0 entries" does not prove that nothing was written. */
static inline void g7nv_summary(G7Nv *n) {
    if (!n) return;
    pthread_mutex_lock(&n->mu);
    if (n->summarized) { pthread_mutex_unlock(&n->mu); return; }
    n->summarized = 1;
    if (!n->on) { fprintf(n->out, "g7-nvram: summary log off (G7_NVRAM_LOG=0)\n"); fflush(n->out); pthread_mutex_unlock(&n->mu); return; }
    char keys[900]; size_t k = 0; keys[0] = 0;
    for (int i = 0; i < G7NV_NKEYS && k + 1 < sizeof keys; i++) {
        if (!n->key[i].seen) k += (size_t)snprintf(keys + k, sizeof keys - k, " %s=never-seen", g7nv_key_names[i]);
        else k += (size_t)snprintf(keys + k, sizeof keys - k, " %s=last:%s/state=0x%02x/size=%u/guid=%s/val32=%s/txt=\"%s\"/set=%u,delete=%u,read=%u", g7nv_key_names[i], n->key[i].op, n->key[i].state, n->key[i].dsz,
                                   n->key[i].guid, n->key[i].hex, n->key[i].txt, n->key[i].n_set, n->key[i].n_del, n->key[i].n_get);
    }
    fprintf(n->out, "g7-nvram: summary aux_reads=%llu (%llu B, failed %llu) aux_writes=%llu (%llu B, failed %llu) aux_flushes=%llu store_headers=%llu entries set=%llu delete=%llu read=%llu lines=%u/%u suppressed=%u dedup_repeats=%u |%s | "
            "counts of what the guest asked of the aux disk (NVRAM lives there); entry recognition is a heuristic [I], not a parser of the guest's own format\n",
            (unsigned long long)n->reads, (unsigned long long)n->rbytes, (unsigned long long)n->rfail, (unsigned long long)n->writes, (unsigned long long)n->wbytes, (unsigned long long)n->wfail, (unsigned long long)n->flushes,
            (unsigned long long)n->hdrs, (unsigned long long)n->sets, (unsigned long long)n->dels, (unsigned long long)n->gets, n->op_lines + n->hit_lines, G7NV_LINE_CAP, n->suppressed, n->repeats, keys);
    fflush(n->out);
    pthread_mutex_unlock(&n->mu);
}

/* ---- S1: opt-in pre-seed ---- */
#define G7NV_SEED_MAX 4
typedef struct { char name[48]; uint8_t val[192]; size_t vlen; } G7NvSeed;

/* Spec: "1"/"on"/"true" = auto-boot=true; otherwise "name=value" items separated by ';' or newlines. Names [A-Za-z0-9._:-], values printable ASCII, not empty. "0"/"off"/"" = off (returns 0 items). Returns the
   item count, or -1 with err set. boot-volume is accepted only as an explicit "boot-volume=<string>" item; there is no default for it. */
static inline int g7nv_parse_spec(const char *spec, G7NvSeed *out, int max, char *err, size_t el) {
    if (!spec) return 0;
    while (*spec == ' ' || *spec == '\t' || *spec == '\n' || *spec == '\r') spec++;
    size_t sl = strlen(spec);
    while (sl && (spec[sl - 1] == ' ' || spec[sl - 1] == '\t' || spec[sl - 1] == '\n' || spec[sl - 1] == '\r')) sl--;
    if (!sl || (sl == 1 && spec[0] == '0') || (sl == 3 && !strncasecmp(spec, "off", 3))) return 0;
    if ((sl == 1 && spec[0] == '1') || (sl == 2 && !strncasecmp(spec, "on", 2)) || (sl == 4 && !strncasecmp(spec, "true", 4))) spec = "auto-boot=true", sl = strlen(spec);
    int cnt = 0;
    size_t i = 0;
    while (i < sl) {
        size_t j = i; while (j < sl && spec[j] != ';' && spec[j] != '\n') j++;
        size_t a = i, b = j;
        while (a < b && (spec[a] == ' ' || spec[a] == '\t' || spec[a] == '\r')) a++;
        while (b > a && (spec[b - 1] == ' ' || spec[b - 1] == '\t' || spec[b - 1] == '\r')) b--;
        i = j + 1;
        if (a == b) continue;
        const char *eq = memchr(spec + a, '=', b - a);
        if (!eq || eq == spec + a) { snprintf(err, el, "item without name=value"); return -1; }
        size_t nl = (size_t)(eq - (spec + a)), vl = b - a - nl - 1;
        if (nl >= sizeof out[0].name) { snprintf(err, el, "name too long"); return -1; }
        if (!vl || vl > sizeof out[0].val - 1) { snprintf(err, el, "value empty or longer than %zu", sizeof out[0].val - 1); return -1; }
        for (size_t q = 0; q < nl; q++) { unsigned char c = (unsigned char)spec[a + q]; if (!isalnum(c) && c != '.' && c != '_' && c != ':' && c != '-') { snprintf(err, el, "bad character in the name"); return -1; } }
        for (size_t q = 0; q < vl; q++) { unsigned char c = (unsigned char)eq[1 + q]; if (c < 0x20 || c > 0x7E) { snprintf(err, el, "value must be printable ASCII"); return -1; } }
        if (cnt >= max) { snprintf(err, el, "more than %d items", max); return -1; }
        memcpy(out[cnt].name, spec + a, nl); out[cnt].name[nl] = 0;
        memcpy(out[cnt].val, eq + 1, vl); out[cnt].vlen = vl;
        for (int p = 0; p < cnt; p++) if (!strcmp(out[p].name, out[cnt].name)) { snprintf(err, el, "name given twice"); return -1; }
        cnt++;
    }
    return cnt;
}

/* Where the request comes from: env1 (G7_NVRAM_PRESEED), then env2 (HENGE_NVRAM_PRESEED), then the marker file's text (file_text, NULL when the file is absent). Writes the winning text to out and returns
   its source name; returns NULL when nothing asks for it (the default). A set environment value wins even when it says off. */
static inline const char *g7nv_spec_source(const char *env1, const char *env2, const char *file_text, char *out, size_t cap) {
    const char *src = NULL, *txt = NULL;
    if (env1) src = "env G7_NVRAM_PRESEED", txt = env1;
    else if (env2) src = "env HENGE_NVRAM_PRESEED", txt = env2;
    else if (file_text) src = "marker file nvram_preseed", txt = file_text;
    if (!src) return NULL;
    snprintf(out, cap, "%s", txt);
    return src;
}

typedef struct { size_t pos; uint8_t state; uint32_t dsz, nsz; const uint8_t *guid; char name[64]; const uint8_t *val; } G7NvPEnt;

/* A candidate store at part[0..plen): "3VVN", version 1, size sane, and the entry chain consistent (valid entries with matching CRC up to an erased/zero header or the store end). */
static inline int g7nv_walk(const uint8_t *part, size_t plen, uint32_t *size, G7NvPEnt *ents, int maxe, int *nents, size_t *end) {
    if (plen < 24 || memcmp(part, "3VVN", 4) || part[14] != 1) return 0;
    uint32_t sz = g7nv_le32(part + 4);
    if (sz < 64 || sz > G7NV_PART || sz > plen) return 0;
    size_t pos = 24; int ne = 0;
    while (pos + 36 < sz) {
        int ff = 1, zz = 1;
        for (int i = 0; i < 36; i++) { if (part[pos + i] != 0xFF) ff = 0; if (part[pos + i] != 0) zz = 0; }
        if (ff || zz) break;
        G7NvEnt e;
        if (!g7nv_parse_entry(part, sz, pos, &e) || !e.complete) return 0;
        if (ne >= maxe) return 0;
        G7NvPEnt *o = &ents[ne++];
        o->pos = pos; o->state = e.state; o->dsz = e.data_size; o->nsz = e.name_size; o->guid = e.guid; o->val = e.val; memcpy(o->name, e.name, sizeof o->name);
        pos += e.total;
    }
    *size = sz; *nents = ne; *end = pos;
    return 1;
}

/* Result of g7nv_preseed. */
typedef struct { int store_found, seeded, already, failed, io_fail; uint64_t store_off, scanned; uint32_t generation; } G7NvSeedResult;   /* io_fail: a write or the final flush failed (the aux may be half changed: see g7nv_preseed_guarded) */

/* Finds the newest valid store in the aux disk (4 KiB steps over the whole disk, read-only) and appends the items. Writes nothing when no valid store is found, when there is no room, or when the
   bytes after the used area are not erased (0xFF). Logs "g7-nvram: preseed ..." lines to out. The caller installs the S0 observer AFTER this (so seeding is not logged as guest traffic). */
static inline G7NvSeedResult g7nv_preseed(HvDisk *d, const G7NvSeed *items, int nitems, FILE *out) {
    G7NvSeedResult r; memset(&r, 0, sizeof r);
    uint8_t *part = malloc(G7NV_PART), *chunk = malloc(1u << 20);
    G7NvPEnt *ents = malloc(sizeof(G7NvPEnt) * 512);
    if (!part || !chunk || !ents) { fprintf(out, "g7-nvram: preseed FAILED out of memory; nothing written\n"); free(part); free(chunk); free(ents); r.failed = nitems; return r; }
    uint64_t best_off = 0; uint32_t best_gen = 0, best_size = 0; int have = 0, best_ne = 0; size_t best_end = 0;
    for (uint64_t base = 0; base < d->size; base += (1u << 20)) {
        uint64_t n = d->size - base < (1u << 20) ? d->size - base : (1u << 20);
        if (hv_disk_read(d, base, chunk, n) != 0) continue;
        r.scanned += n;
        for (uint64_t o = 0; o + 24 <= n; o += 4096) {
            if (memcmp(chunk + o, "3VVN", 4)) continue;
            uint64_t at = base + o;
            uint64_t pl = d->size - at < G7NV_PART ? d->size - at : G7NV_PART;
            if (hv_disk_read(d, at, part, pl) != 0) continue;
            uint32_t sz; int ne; size_t end;
            if (!g7nv_walk(part, (size_t)pl, &sz, ents, 512, &ne, &end)) continue;
            uint32_t gen = g7nv_le32(part + 8);
            if (!have || gen > best_gen) { have = 1; best_off = at; best_gen = gen; best_size = sz; best_ne = ne; best_end = end; }
        }
    }
    if (!have) {
        fprintf(out, "g7-nvram: preseed requested but NO valid v3 \"3VVN\" store found in the aux disk (scanned %llu bytes at 4 KiB steps); NOTHING WRITTEN [N: where this guest keeps its NVRAM is not known; the S0 lines show where it writes]\n", (unsigned long long)r.scanned);
        r.failed = nitems; free(part); free(chunk); free(ents); fflush(out); return r;
    }
    r.store_found = 1; r.store_off = best_off; r.generation = best_gen;
    uint64_t pl = d->size - best_off < G7NV_PART ? d->size - best_off : G7NV_PART;
    uint32_t sz; int ne; size_t end;
    if (hv_disk_read(d, best_off, part, pl) != 0 || !g7nv_walk(part, (size_t)pl, &sz, ents, 512, &ne, &end)) { fprintf(out, "g7-nvram: preseed FAILED to re-read the store; nothing written\n"); r.failed = nitems; free(part); free(chunk); free(ents); return r; }
    (void)best_ne; (void)best_end; (void)best_size;
    fprintf(out, "g7-nvram: preseed store found at aux_off=0x%llx generation=%u size=%u entries=%d used_end=%zu\n", (unsigned long long)best_off, best_gen, sz, ne, end);
    for (int v = 0; v < nitems; v++) {
        const G7NvSeed *it = &items[v];
        const uint8_t *guid = g7nv_is_system_name(it->name) ? g7nv_guid_system : g7nv_guid_common;
        int same = 0;
        for (int e = 0; e < ne; e++) if (ents[e].state == 0x7F && !memcmp(ents[e].guid, guid, 16) && !strcmp(ents[e].name, it->name) && ents[e].dsz == it->vlen && !memcmp(ents[e].val, it->val, it->vlen)) same = 1;
        if (same) { fprintf(out, "g7-nvram: preseed %s already has the requested value; unchanged\n", it->name); r.already++; continue; }
        size_t nsz = strlen(it->name) + 1, total = 36 + nsz + it->vlen;
        int erased = end + total <= sz;
        for (size_t q = 0; erased && q < total; q++) if (part[end + q] != 0xFF) erased = 0;
        if (!erased) { fprintf(out, "g7-nvram: preseed %s NOT written: no room or the bytes after the used area are not erased (this code does not rotate banks)\n", it->name); r.failed++; continue; }
        uint8_t *ent = calloc(1, total);
        if (!ent) { r.failed++; continue; }
        ent[0] = 0xAA; ent[1] = 0x55; ent[2] = 0x7F; ent[3] = 0; g7nv_put32(ent + 4, 0); g7nv_put32(ent + 8, (uint32_t)nsz); g7nv_put32(ent + 12, (uint32_t)it->vlen);
        memcpy(ent + 16, guid, 16); g7nv_put32(ent + 32, g7nv_crc32(it->val, it->vlen)); memcpy(ent + 36, it->name, nsz - 1); memcpy(ent + 36 + nsz, it->val, it->vlen);
        int wr = G7NV_DISK_WRITE(d, best_off + end, ent, total);
        free(ent);
        if (wr != 0) { fprintf(out, "g7-nvram: preseed %s FAILED to write (rc %d)\n", it->name, wr); r.failed++; r.io_fail = 1; break; }   /* integ7b: stop at the first failed write: nothing more is written after an I/O error */
        /* the old copies are marked deleted only AFTER the new one is on disk */
        int inval = 0;
        for (int e = 0; e < ne; e++) if (ents[e].state == 0x7F && !memcmp(ents[e].guid, guid, 16) && !strcmp(ents[e].name, it->name)) {
            uint8_t st = 0x7C;
            if (G7NV_DISK_WRITE(d, best_off + ents[e].pos + 2, &st, 1) == 0) { inval++; ents[e].state = 0x7C; } else { r.io_fail = 1; fprintf(out, "g7-nvram: preseed %s: marking the older copy deleted FAILED\n", it->name); }
        }
        char gt[40];
        fprintf(out, "g7-nvram: preseed %s=%.*s written (guid=%s, %zu bytes entry at aux_off=0x%llx, %d older copy/copies marked deleted) [N: value encoding the guest expects is not verified]\n", it->name,
                (int)(it->vlen > 40 ? 40 : it->vlen), (const char *)it->val, g7nv_guid_name(guid, gt, sizeof gt), total, (unsigned long long)(best_off + end), inval);
        r.seeded++;
        /* re-read the store so the next item starts after this entry */
        if (hv_disk_read(d, best_off, part, pl) != 0 || !g7nv_walk(part, (size_t)pl, &sz, ents, 512, &ne, &end)) { fprintf(out, "g7-nvram: preseed stopped: could not re-read the store\n"); r.failed += nitems - v - 1; break; }
    }
    if (r.seeded || r.io_fail) { int fr = G7NV_DISK_FLUSH(d); if (fr != 0) { r.io_fail = 1; fprintf(out, "g7-nvram: preseed flush FAILED (rc %d): the writes may not be on disk\n", fr); } }
    fprintf(out, "g7-nvram: preseed result seeded=%d already=%d failed=%d (host-side writes only; whether the guest honours them is shown by the S0 lines of this run)\n", r.seeded, r.already, r.failed);
    fflush(out);
    free(part); free(chunk); free(ents);
    return r;
}

/* ---- integ7b T4: the guards around S1 (fail closed) ----
   (a) before the first write the guest-visible aux image is copied to a sibling backup file (never overwritten: O_EXCL, a numbered name when taken), checked for free space first, read back and compared;
       a backup that cannot be made, verified, or does not fit means NOTHING is written.
   (b) the marker file is read whole or not at all: unreadable, too long (would be cut), with a NUL / non-printable byte = refused with the reason; absent = off.
   (c) a failed or short write or a failed flush: the store region is put back from the backup and verified; if that cannot be done the result says possibly_modified, the log says loudly "restore from <backup>",
       and the runner does not start the machine.
   (d) only the verified disposable clone: the caller says so (target_ok, from hv_clone_open), the backup name must sit beside a clones/clone-NNN folder; otherwise nothing is written. */
typedef struct {
    int target_ok;              /* the aux disk is the verified clone's (caller: hv_clone_open succeeded); 0 = refuse */
    const char *clone_real;     /* the clone folder's real path: <vm>/clones/clone-NNN */
} G7NvGuard;
typedef struct { G7NvSeedResult seed; int refused, backed_up, restored, possibly_modified; char backup[1100]; uint64_t backup_bytes; } G7NvGuardResult;

/* 1: ok (buf holds the text, NUL ended). 0: absent. -1: refused, why says what. */
static inline int g7nv_read_marker(const char *path, char *buf, size_t cap, char *why, size_t wl) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { if (errno == ENOENT) return 0; snprintf(why, wl, "marker unreadable: open: %s", strerror(errno)); return -1; }
    size_t n = 0; ssize_t k;
    while (n < cap - 1 && (k = read(fd, buf + n, cap - 1 - n)) != 0) { if (k < 0) { if (errno == EINTR) continue; snprintf(why, wl, "marker unreadable: read: %s", strerror(errno)); close(fd); return -1; } n += (size_t)k; }
    char extra; ssize_t more = read(fd, &extra, 1);
    close(fd);
    if (more < 0) { snprintf(why, wl, "marker unreadable: read: %s", strerror(errno)); return -1; }
    if (more > 0) { snprintf(why, wl, "marker longer than %zu bytes (would be cut: partial content is not used)", cap - 1); return -1; }
    buf[n] = 0;
    for (size_t i = 0; i < n; i++) { unsigned char c = (unsigned char)buf[i]; if (c == 0 || (c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c >= 0x7f) { snprintf(why, wl, "marker has a NUL or non-printable byte at %zu", i); return -1; } }
    return 1;
}

static inline int g7nv_make_backup(HvDisk *d, const char *clone_real, char *bpath, size_t bcap, uint64_t *bytes, char *why, size_t wl) {
    const char *base = strrchr(clone_real, '/'); base = base ? base + 1 : clone_real;
    char dirp[1024]; size_t dl = base - clone_real; if (dl == 0 || dl >= sizeof dirp) { snprintf(why, wl, "clone path unusable"); return -1; }
    snprintf(dirp, sizeof dirp, "%.*s", (int)(dl - 1), clone_real);
    uint64_t need = d->size + (1u << 20);
    char probe[1100]; snprintf(probe, sizeof probe, "%s/x", dirp);
    uint64_t freeb = G7NV_FREE_BYTES(probe);
    if (freeb < need) { snprintf(why, wl, "not enough free space for the backup (need %llu bytes, free %llu)", (unsigned long long)need, (unsigned long long)freeb); return -1; }
    int fd = -1;
    for (int i = 0; i < 20 && fd < 0; i++) {
        if (i == 0) snprintf(bpath, bcap, "%s/%s.aux-preseed-backup.img", dirp, base); else snprintf(bpath, bcap, "%s/%s.aux-preseed-backup-%d.img", dirp, base, i + 1);
        fd = open(bpath, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST) { snprintf(why, wl, "cannot create the backup %s: %s", bpath, strerror(errno)); return -1; }
    }
    if (fd < 0) { snprintf(why, wl, "20 backups already exist beside the clone; delete old ones"); return -1; }
    uint8_t *buf = malloc(1u << 20);
    if (!buf) { close(fd); unlink(bpath); snprintf(why, wl, "out of memory"); return -1; }
    int bad = 0;
    for (uint64_t o = 0; o < d->size && !bad; o += (1u << 20)) {
        size_t n = d->size - o < (1u << 20) ? (size_t)(d->size - o) : (1u << 20);
        if (hv_disk_read(d, o, buf, n) != 0) { snprintf(why, wl, "cannot read the aux image at %llu", (unsigned long long)o); bad = 1; break; }
        size_t w = 0; while (w < n) { ssize_t k = write(fd, buf + w, n - w); if (k < 0) { if (errno == EINTR) continue; snprintf(why, wl, "backup write failed at %llu: %s", (unsigned long long)(o + w), strerror(errno)); bad = 1; break; } if (k == 0) { snprintf(why, wl, "backup write made no progress"); bad = 1; break; } w += (size_t)k; }
    }
    if (!bad && fsync(fd) != 0) { snprintf(why, wl, "backup fsync failed: %s", strerror(errno)); bad = 1; }
    if (close(fd) != 0 && !bad) { snprintf(why, wl, "backup close failed: %s", strerror(errno)); bad = 1; }
    if (!bad) {   /* read it back and compare with the disk */
        int rf = open(bpath, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); uint8_t *b2 = malloc(1u << 20);
        if (rf < 0 || !b2) { snprintf(why, wl, "cannot reopen the backup to verify it"); bad = 1; }
        for (uint64_t o = 0; !bad && o < d->size; o += (1u << 20)) {
            size_t n = d->size - o < (1u << 20) ? (size_t)(d->size - o) : (1u << 20);
            if (hv_disk_read(d, o, buf, n) != 0 || pread(rf, b2, n, (off_t)o) != (ssize_t)n || memcmp(buf, b2, n) != 0) { snprintf(why, wl, "the backup does not match the aux image at %llu", (unsigned long long)o); bad = 1; }
        }
        if (rf >= 0) close(rf);
        free(b2);
    }
    free(buf);
    if (bad) { unlink(bpath); bpath[0] = 0; return -1; }
    *bytes = d->size;
    return 0;
}

/* Puts [off, off+len) of the aux image back from the backup file and verifies it. 0 on success. */
static inline int g7nv_restore_region(HvDisk *d, const char *bpath, uint64_t off, uint64_t len) {
    int rf = open(bpath, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); if (rf < 0) return -1;
    uint8_t *a = malloc(len), *b = malloc(len); int rc = -1;
    if (a && b && pread(rf, a, len, (off_t)off) == (ssize_t)len && G7NV_DISK_WRITE(d, off, a, len) == 0 && G7NV_DISK_FLUSH(d) == 0 && hv_disk_read(d, off, b, len) == 0 && !memcmp(a, b, len)) rc = 0;
    free(a); free(b); close(rf);
    return rc;
}

static inline G7NvGuardResult g7nv_preseed_guarded(HvDisk *d, const G7NvSeed *items, int nitems, FILE *out, const G7NvGuard *g) {
    G7NvGuardResult r; memset(&r, 0, sizeof r);
    char why[1300] = "";
    if (!g || !g->target_ok || !g->clone_real || !strstr(g->clone_real, "/clones/clone-")) { fprintf(out, "g7-nvram: preseed REFUSED: the target is not the verified disposable clone; nothing written\n"); r.refused = 1; r.seed.failed = nitems; fflush(out); return r; }
    if (!d || d->readonly || d->size < G7NV_PART) { fprintf(out, "g7-nvram: preseed REFUSED: the aux disk is not writable here or too small; nothing written\n"); r.refused = 1; r.seed.failed = nitems; fflush(out); return r; }
    if (g7nv_make_backup(d, g->clone_real, r.backup, sizeof r.backup, &r.backup_bytes, why, sizeof why) != 0) {
        fprintf(out, "g7-nvram: preseed REFUSED: no safe backup of the aux image before writing (%s); NOTHING WRITTEN\n", why); r.refused = 1; r.seed.failed = nitems; fflush(out); return r;
    }
    r.backed_up = 1;
    fprintf(out, "g7-nvram: preseed backup of the aux image written and verified: %s (%llu bytes); restore with: cp over the clone's macos_aux.img only if the clone is to be reset (the first 16 KiB hidden metadata is not part of this copy)\n", r.backup, (unsigned long long)r.backup_bytes);
    r.seed = g7nv_preseed(d, items, nitems, out);
    if (r.seed.io_fail) {
        uint64_t off = r.seed.store_found ? r.seed.store_off : 0, len = r.seed.store_found ? G7NV_PART : 0;
        if (len && off + len > d->size) len = d->size - off;
        if (len && g7nv_restore_region(d, r.backup, off, len) == 0) { r.restored = 1; fprintf(out, "g7-nvram: preseed I/O error: the NVRAM store region (aux_off=0x%llx, %llu bytes) was put back from %s and verified; the aux image is as before the pre-seed\n", (unsigned long long)off, (unsigned long long)len, r.backup); }
        else { r.possibly_modified = 1; fprintf(out, "g7-nvram: !!! PRESEED I/O ERROR AND THE RESTORE FAILED: the aux image is POSSIBLY MODIFIED. Restore from %s (the machine will not be started) !!!\n", r.backup); }
    }
    fflush(out);
    return r;
}

#endif
