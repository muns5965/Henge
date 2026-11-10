#define _GNU_SOURCE 1
#include "remote_zip.h"
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zip.h>
#include "curl/curl.h"

#define SLOTS 4
typedef struct { uint64_t start; size_t len; uint8_t *data; uint64_t used; } Slot;

struct RemoteZip {
    char *url;
    RzLimits lim;
    RzStats st;
    char etag[160];              /* strong ETag, with its quotes, or "" */
    int weak_etag_seen;
    char last_modified[80];
    Slot slot[SLOTS];
    uint64_t tick;
    char err[256];               /* first failure, kept for the zip layer */
    FILE *trace;
    zip_t *za;
};

/* ---- one Range request ---- */
typedef struct {
    RemoteZip *z;
    int suffix;                  /* request was "bytes=-N" */
    uint64_t want_start, want_end;   /* inclusive; for a suffix request unknown until the reply */
    uint64_t max_len;            /* a suffix reply may not be longer than this */
    int status, headers_done, refused;
    char reason[200];
    uint64_t cr_start, cr_end, cr_total; int have_cr;
    char r_etag[160], r_lm[80];
    int identity;
    uint8_t *buf; size_t got, cap;
    uint64_t wire;
} Req;

static void seterr(char *dst, size_t cap, const char *fmt, ...) {
    if (!cap) return;
    va_list ap; va_start(ap, fmt); vsnprintf(dst, cap, fmt, ap); va_end(ap);
}
static void fail(RemoteZip *z, const char *fmt, ...) {
    if (z->err[0]) return;
    va_list ap; va_start(ap, fmt); vsnprintf(z->err, sizeof z->err, fmt, ap); va_end(ap);
}
static void refuse(Req *r, const char *fmt, ...) {
    if (r->refused) return;
    r->refused = 1;
    va_list ap; va_start(ap, fmt); vsnprintf(r->reason, sizeof r->reason, fmt, ap); va_end(ap);
}

static int url_ok(const char *u) {
    if (!strncasecmp(u, "https://", 8)) return 1;
#ifdef HV_RZ_ALLOW_HTTP_TEST
    if (!strncasecmp(u, "http://127.0.0.1", 16) || !strncasecmp(u, "http://localhost", 16)) return 1;
#endif
    return 0;
}

static void trim(char *s) { size_t n = strlen(s); while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ')) s[--n] = 0; }

static int finish_headers(Req *r) {
    RemoteZip *z = r->z;
    r->headers_done = 1;
    if (r->status != 206) { refuse(r, "server answered %d, not 206: Range ignored%s", r->status, z->etag[0] ? " or the file changed (If-Range failed)" : ""); return -1; }
    if (!r->identity) { refuse(r, "reply is content-encoded"); return -1; }
    if (!r->have_cr) { refuse(r, "206 without a usable Content-Range"); return -1; }
    if (r->cr_end < r->cr_start || r->cr_end >= r->cr_total) { refuse(r, "inconsistent Content-Range"); return -1; }
    if (z->st.size && r->cr_total != z->st.size) { refuse(r, "file size changed (%llu -> %llu)", (unsigned long long)z->st.size, (unsigned long long)r->cr_total); return -1; }
    if (r->suffix) {
        if (r->cr_end != r->cr_total - 1) { refuse(r, "suffix reply does not end at the end of the file"); return -1; }
        if (r->cr_end - r->cr_start + 1 > r->max_len) { refuse(r, "suffix reply longer than requested"); return -1; }
    } else {
        uint64_t want_end = r->want_end < r->cr_total - 1 ? r->want_end : r->cr_total - 1;
        if (r->cr_start != r->want_start || r->cr_end != want_end) { refuse(r, "Content-Range does not match the request"); return -1; }
    }
    if (z->etag[0]) {
        if (strcmp(r->r_etag, z->etag)) { refuse(r, "ETag changed: the file changed"); return -1; }
    } else if (z->last_modified[0] && strcmp(r->r_lm, z->last_modified)) { refuse(r, "Last-Modified changed: the file changed"); return -1; }
    return 0;
}

static size_t hdr_cb(char *p, size_t s, size_t n, void *ud) {
    Req *r = ud; size_t tot = s * n;
    char line[400];
    if (r->refused) return tot;
    size_t m = tot < sizeof line - 1 ? tot : sizeof line - 1;
    memcpy(line, p, m); line[m] = 0;
    trim(line);
    if (!strncmp(line, "HTTP/", 5)) { /* a new status line: forget earlier headers (redirect hops) */
        const char *sp = strchr(line, ' ');
        r->status = sp ? atoi(sp + 1) : 0;
        r->have_cr = 0; r->r_etag[0] = r->r_lm[0] = 0; r->identity = 1; r->headers_done = 0;
    } else if (!line[0]) {
        if (!r->headers_done && r->status >= 200 && r->status / 100 != 3) finish_headers(r);   /* a redirect's own headers say nothing yet */
    } else if (!strncasecmp(line, "Content-Range:", 14)) {
        unsigned long long a, b, t;
        if (sscanf(line + 14, " bytes %llu-%llu/%llu", &a, &b, &t) == 3) { r->cr_start = a; r->cr_end = b; r->cr_total = t; r->have_cr = 1; }
    } else if (!strncasecmp(line, "ETag:", 5)) {
        const char *v = line + 5; while (*v == ' ') v++;
        if (!strncmp(v, "W/", 2)) r->z->weak_etag_seen = 1;    /* weak: never a validator */
        else snprintf(r->r_etag, sizeof r->r_etag, "%s", v);
    } else if (!strncasecmp(line, "Last-Modified:", 14)) {
        const char *v = line + 14; while (*v == ' ') v++;
        snprintf(r->r_lm, sizeof r->r_lm, "%s", v);
    } else if (!strncasecmp(line, "Content-Encoding:", 17)) {
        const char *v = line + 17; while (*v == ' ') v++;
        if (strcasecmp(v, "identity")) r->identity = 0;
    }
    return tot;
}

static size_t body_cb(char *p, size_t s, size_t n, void *ud) {
    Req *r = ud; size_t tot = s * n;
    r->wire += tot; r->z->st.wire_bytes += tot;
    if (!r->headers_done) finish_headers(r);   /* a server that sends no blank line is not worth waiting for */
    if (r->refused) return 0;                   /* ends the transfer: a 200 with a gigabyte body costs one buffer, not a download */
    if (r->z->lim.cancel && *r->z->lim.cancel) { refuse(r, "cancelled"); return 0; }
    uint64_t expect = r->cr_end - r->cr_start + 1;
    if (r->got + tot > expect) { refuse(r, "more body than the Content-Range announced"); return 0; }
    if (r->got + tot > r->cap) { refuse(r, "body larger than the buffer"); return 0; }
    memcpy(r->buf + r->got, p, tot); r->got += tot;
    return tot;
}

static void flush(RemoteZip *z) { for (int i = 0; i < SLOTS; i++) { free(z->slot[i].data); z->slot[i].data = NULL; z->slot[i].len = 0; } }

/* Fetch [start, end] (or the last `suffix_len` bytes) into a new slot. */
static Slot *fetch(RemoteZip *z, int suffix, uint64_t start, uint64_t end, uint64_t suffix_len) {
    if (z->st.broken) { fail(z, "the remote file is no longer trusted (%s)", z->err[0] ? z->err : "earlier failure"); return NULL; }
    if (z->lim.cancel && *z->lim.cancel) { fail(z, "cancelled"); return NULL; }
    uint64_t maxlen = suffix ? suffix_len : end - start + 1;
    if (z->st.requests >= z->lim.max_requests) { fail(z, "request limit (%u) reached", z->lim.max_requests); return NULL; }
    if (z->st.bytes + (suffix ? 1 : maxlen) > z->lim.max_bytes) { fail(z, "byte limit (%llu) reached", (unsigned long long)z->lim.max_bytes); return NULL; }
    Req r; memset(&r, 0, sizeof r);
    r.z = z; r.suffix = suffix; r.want_start = start; r.want_end = end; r.max_len = maxlen; r.identity = 1;
    r.cap = (size_t)maxlen; r.buf = malloc(r.cap ? r.cap : 1);
    if (!r.buf) { fail(z, "out of memory"); return NULL; }
    z->st.requests++;
    CURL *h = curl_easy_init();
    if (!h) { free(r.buf); fail(z, "curl init failed"); return NULL; }
    char rng[96], ifr[200];
    if (suffix) snprintf(rng, sizeof rng, "Range: bytes=-%llu", (unsigned long long)suffix_len);
    else snprintf(rng, sizeof rng, "Range: bytes=%llu-%llu", (unsigned long long)start, (unsigned long long)end);
    struct curl_slist *hd = curl_slist_append(NULL, rng);
    hd = curl_slist_append(hd, "Accept-Encoding: identity");
    if (z->etag[0]) { snprintf(ifr, sizeof ifr, "If-Range: %s", z->etag); hd = curl_slist_append(hd, ifr); }
    char errbuf[CURL_ERROR_SIZE]; errbuf[0] = 0;
    curl_easy_setopt(h, CURLOPT_URL, z->url);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hd);
    curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);   /* the stand-in refuses https -> http */
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, (curl_write_callback)hdr_cb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &r);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, (curl_write_callback)body_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &r);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, z->lim.connect_timeout_s);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, z->lim.request_timeout_s);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "Henge-remote-zip/1");
    CURLcode rc = curl_easy_perform(h);
    curl_slist_free_all(hd);
    curl_easy_cleanup(h);
    if (z->trace) fprintf(z->trace, "REQ %s status=%d got=%zu wire=%llu%s%s\n", rng + 7, r.status, r.got, (unsigned long long)r.wire, r.refused ? " REFUSED: " : "", r.refused ? r.reason : "");
    if (!r.refused && rc == CURLE_OK && !r.headers_done) finish_headers(&r);
    if (!r.refused && rc != CURLE_OK) refuse(&r, "transfer failed: %s", errbuf[0] ? errbuf : "curl error");
    if (!r.refused && r.got != r.cr_end - r.cr_start + 1) refuse(&r, "short body (%zu of %llu bytes)", r.got, (unsigned long long)(r.cr_end - r.cr_start + 1));
    if (r.refused) {
        int changed = strstr(r.reason, "changed") != NULL || r.status == 200;
        if (changed || r.status == 206) { z->st.broken = 1; flush(z); }   /* a reply we cannot trust poisons the cache */
        fail(z, "%s", r.reason);
        free(r.buf);
        return NULL;
    }
    if (z->st.bytes + r.got > z->lim.max_bytes) { fail(z, "byte limit (%llu) reached", (unsigned long long)z->lim.max_bytes); free(r.buf); return NULL; }
    /* accepted */
    if (!z->st.size) {
        if (r.cr_total > z->lim.max_file_size) { fail(z, "remote file is %llu bytes, over the limit", (unsigned long long)r.cr_total); free(r.buf); return NULL; }
        z->st.size = r.cr_total;
        if (r.r_etag[0]) { snprintf(z->etag, sizeof z->etag, "%s", r.r_etag); z->st.strong_etag = 1; }
        else { snprintf(z->last_modified, sizeof z->last_modified, "%s", r.r_lm); z->st.consistency_weak = 1; }
        if (z->lim.require_strong_etag && !z->st.strong_etag) { fail(z, "no strong ETag%s: refusing (require_strong_etag)", z->weak_etag_seen ? " (only a weak one)" : ""); free(r.buf); return NULL; }
    }
    z->st.bytes += r.got;
    Slot *s = NULL;
    for (int i = 0; i < SLOTS; i++) if (!z->slot[i].data) { s = &z->slot[i]; break; }
    if (!s) { s = &z->slot[0]; for (int i = 1; i < SLOTS; i++) if (z->slot[i].used < s->used) s = &z->slot[i]; free(s->data); }
    s->data = r.buf; s->len = r.got; s->start = r.cr_start; s->used = ++z->tick;
    return s;
}

int rz_read_at(RemoteZip *z, uint64_t off, void *buf, size_t n, char *err, size_t errcap) {
    uint8_t *o = buf;
    if (n && z->st.size && (off > z->st.size || n > z->st.size - off)) { seterr(err, errcap, "read of %zu bytes at %llu is beyond the end of the file (%llu bytes)", n, (unsigned long long)off, (unsigned long long)z->st.size); return -1; }
    if (n > z->lim.max_bytes) { seterr(err, errcap, "read of %zu bytes exceeds the byte limit", n); return -1; }
    while (n) {
        Slot *s = NULL;
        for (int i = 0; i < SLOTS; i++) if (z->slot[i].data && off >= z->slot[i].start && off < z->slot[i].start + z->slot[i].len) { s = &z->slot[i]; break; }
        if (!s) {
            uint64_t bs = z->lim.block_size, a = off / bs * bs, b = a + bs - 1;
            if (z->st.size && b > z->st.size - 1) b = z->st.size - 1;
            s = fetch(z, 0, a, b, 0);
            if (!s) { seterr(err, errcap, "%s", z->err); return -1; }
        }
        s->used = ++z->tick;
        size_t k = (size_t)(s->start + s->len - off); if (k > n) k = n;
        memcpy(o, s->data + (off - s->start), k);
        o += k; off += k; n -= k;
    }
    return 0;
}

/* ---- libzip source ---- */
typedef struct { RemoteZip *z; uint64_t pos; zip_error_t error; } Src;

static zip_int64_t src_cb(void *state, void *data, zip_uint64_t len, zip_source_cmd_t cmd) {
    Src *s = state; RemoteZip *z = s->z;
    switch (cmd) {
    case ZIP_SOURCE_OPEN: s->pos = 0; return 0;
    case ZIP_SOURCE_READ: {
        if (!z->st.size) { /* first touch: learn the size and validators from the tail */
            if (!fetch(z, 1, 0, 0, z->lim.block_size)) { zip_error_set(&s->error, ZIP_ER_READ, EIO); return -1; }
        }
        uint64_t left = z->st.size > s->pos ? z->st.size - s->pos : 0;
        size_t n = len < left ? (size_t)len : (size_t)left;
        char why[256];
        if (rz_read_at(z, s->pos, data, n, why, sizeof why)) { fail(z, "%s", why); zip_error_set(&s->error, ZIP_ER_READ, EIO); return -1; }
        s->pos += n; return (zip_int64_t)n;
    }
    case ZIP_SOURCE_CLOSE: return 0;
    case ZIP_SOURCE_STAT: {
        if (!z->st.size && !fetch(z, 1, 0, 0, z->lim.block_size)) { zip_error_set(&s->error, ZIP_ER_READ, EIO); return -1; }
        zip_stat_t *st = data; zip_stat_init(st); st->valid |= ZIP_STAT_SIZE; st->size = z->st.size; return sizeof *st;
    }
    case ZIP_SOURCE_ERROR: return zip_error_to_data(&s->error, data, len);
    case ZIP_SOURCE_FREE: free(s); return 0;
    case ZIP_SOURCE_SEEK: {
        if (!z->st.size && !fetch(z, 1, 0, 0, z->lim.block_size)) { zip_error_set(&s->error, ZIP_ER_READ, EIO); return -1; }
        zip_int64_t np = zip_source_seek_compute_offset(s->pos, z->st.size, data, len, &s->error);
        if (np < 0) return -1;
        s->pos = (uint64_t)np; return 0;
    }
    case ZIP_SOURCE_TELL: return (zip_int64_t)s->pos;
    case ZIP_SOURCE_SUPPORTS: return zip_source_make_command_bitmap(ZIP_SOURCE_OPEN, ZIP_SOURCE_READ, ZIP_SOURCE_CLOSE, ZIP_SOURCE_STAT, ZIP_SOURCE_ERROR, ZIP_SOURCE_FREE, ZIP_SOURCE_SEEK, ZIP_SOURCE_TELL, ZIP_SOURCE_SUPPORTS, -1);
    default: return -1;
    }
}

void rz_limits_default(RzLimits *l) {
    memset(l, 0, sizeof *l);
    l->block_size = 256 * 1024; l->max_requests = 24; l->max_bytes = 4u << 20; l->max_entry_bytes = 4u << 20;
    l->max_entries = 20000; l->max_file_size = 64ULL << 30; l->request_timeout_s = 30; l->connect_timeout_s = 15;
}

RemoteZip *rz_open(const char *url, const RzLimits *lim, char *err, size_t errcap) {
    if (!url_ok(url)) { seterr(err, errcap, "refusing a URL that is not https"); return NULL; }
    RemoteZip *z = calloc(1, sizeof *z);
    if (!z) return NULL;
    z->url = strdup(url);
    if (lim) z->lim = *lim; else rz_limits_default(&z->lim);
    z->trace = z->lim.trace;
    if (z->lim.block_size < 4096 || (z->lim.block_size & (z->lim.block_size - 1))) z->lim.block_size = 256 * 1024;
    Src *s = calloc(1, sizeof *s); s->z = z; zip_error_init(&s->error);
    zip_error_t ze; zip_error_init(&ze);
    zip_source_t *zs = zip_source_function_create(src_cb, s, &ze);
    if (!zs) { seterr(err, errcap, "zip source: %s", zip_error_strerror(&ze)); free(s); free(z->url); free(z); return NULL; }
    z->za = zip_open_from_source(zs, ZIP_RDONLY, &ze);
    if (!z->za) {
        seterr(err, errcap, "%s%s%s", z->err[0] ? z->err : "not a readable zip", z->err[0] ? "" : ": ", z->err[0] ? "" : zip_error_strerror(&ze));
        zip_source_free(zs); flush(z); free(z->url); free(z); return NULL;
    }
    zip_int64_t ne = zip_get_num_entries(z->za, 0);
    if (ne < 0 || (uint64_t)ne > z->lim.max_entries) { seterr(err, errcap, "central directory has %lld entries (limit %u)", (long long)ne, z->lim.max_entries); rz_close(z); return NULL; }
    return z;
}

int rz_read_entry(RemoteZip *z, const char *name, uint8_t **out, size_t *len, char *err, size_t errcap) {
    *out = NULL; *len = 0;
    zip_int64_t idx = zip_name_locate(z->za, name, ZIP_FL_ENC_RAW);
    if (idx < 0) { seterr(err, errcap, "no entry named %s", name); return -1; }
    zip_stat_t st; zip_stat_init(&st);
    if (zip_stat_index(z->za, (zip_uint64_t)idx, 0, &st) || !(st.valid & ZIP_STAT_SIZE)) { seterr(err, errcap, "cannot stat %s", name); return -1; }
    if (st.valid & ZIP_STAT_ENCRYPTION_METHOD && st.encryption_method != ZIP_EM_NONE) { seterr(err, errcap, "entry is encrypted"); return -1; }
    if (st.size > z->lim.max_entry_bytes) { seterr(err, errcap, "entry declares %llu bytes (limit %llu)", (unsigned long long)st.size, (unsigned long long)z->lim.max_entry_bytes); return -1; }
    uint8_t *b = malloc((size_t)st.size + 1);
    if (!b) { seterr(err, errcap, "out of memory"); return -1; }
    zip_file_t *f = zip_fopen_index(z->za, (zip_uint64_t)idx, 0);
    if (!f) { seterr(err, errcap, "%s", z->err[0] ? z->err : zip_strerror(z->za)); free(b); return -1; }
    size_t got = 0;
    for (;;) {
        zip_int64_t r = zip_fread(f, b + got, (zip_uint64_t)(st.size + 1 - got));
        if (r < 0) { seterr(err, errcap, "%s", z->err[0] ? z->err : zip_file_strerror(f)); zip_fclose(f); free(b); return -1; }
        if (r == 0) break;
        got += (size_t)r;
        if (got > st.size) { seterr(err, errcap, "entry produces more data than the %llu bytes it declares", (unsigned long long)st.size); zip_fclose(f); free(b); return -1; }
    }
    int cr = zip_fclose(f);
    if (cr) { seterr(err, errcap, "%s", cr == ZIP_ER_CRC ? "CRC-32 mismatch" : "read error at close"); free(b); return -1; }
    if (got != st.size) { seterr(err, errcap, "entry is shorter (%zu) than declared (%llu)", got, (unsigned long long)st.size); free(b); return -1; }
    *out = b; *len = got;
    return 0;
}

const RzStats *rz_stats(const RemoteZip *z) { return &z->st; }
void rz_set_trace(RemoteZip *z, FILE *f) { z->trace = f; }
void rz_close(RemoteZip *z) {
    if (!z) return;
    if (z->za) zip_discard(z->za);
    flush(z); free(z->url); free(z);
}
