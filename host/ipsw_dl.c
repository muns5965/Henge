#define _GNU_SOURCE 1
#include "ipsw_dl.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include "curl/curl.h"
#include "mbedtls/sha256.h"

#define PATHCAP 1200
typedef struct {
    const DlCfg *c;
    char part[PATHCAP], meta[PATHCAP], metatmp[PATHCAP], lockp[PATHCAP], fin[PATHCAP];
    int lockfd, fd;
    mbedtls_sha256_context sha;
    uint64_t committed;           /* bytes in .part, hashed and (after commit) durable */
    uint64_t durable;             /* "committed" as stored in the metadata */
    char etag[160];
    uint64_t commit_every, reserve;
    long long last_prog_ns;
    int meta_broken;              /* a metadata write failed */
} Dl;

static long long now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000000000LL + t.tv_nsec; }
static void lg(const Dl *d, const char *fmt, ...) {
    if (!d->c->out) return;
    va_list ap; va_start(ap, fmt);
    fputs("dl: ", d->c->out); vfprintf(d->c->out, fmt, ap); fputc('\n', d->c->out); fflush(d->c->out);
    va_end(ap);
}
static void setmsg(char *dst, size_t cap, const char *fmt, ...) {
    if (!cap) return;
    va_list ap; va_start(ap, fmt); vsnprintf(dst, cap, fmt, ap); va_end(ap);
}
#define LLU(x) ((unsigned long long)(x))

void dl_cfg_default(DlCfg *c) {
    memset(c, 0, sizeof *c);
    c->reserve = 512ULL << 20; c->commit_every = 16ULL << 20; c->connect_timeout_s = 20;
}

static int url_ok(const char *u) {
    if (!strncasecmp(u, "https://", 8)) return 1;
#ifdef HV_DL_ALLOW_HTTP_TEST
    if (!strncasecmp(u, "http://127.0.0.1", 16) || !strncasecmp(u, "http://localhost", 16)) return 1;
#endif
    return 0;
}
static int is_hex64(const char *s) {
    if (!s || strlen(s) != 64) return 0;
    for (int i = 0; i < 64; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return 0;
    return 1;
}
static uint64_t fnv(const char *s, size_t n) { uint64_t h = 1469598103934665603ULL; for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; } return h; }

static int fsync_dir(const char *dir) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -1;
    int r = fsync(fd); close(fd); return r;
}

/* ---- metadata ---- */
static int meta_write(Dl *d) {
    char body[900], all[1000];
    int n = snprintf(body, sizeof body, "HENGE-DL 1\nurl=%s\nsize=%llu\netag=%s\ncommitted=%llu\n", d->c->url, LLU(d->c->expect_size), d->etag, LLU(d->committed));
    if (n <= 0 || (size_t)n >= sizeof body) return -1;
    int m = snprintf(all, sizeof all, "%scheck=%016llx\n", body, LLU(fnv(body, (size_t)n)));
    int fd = open(d->metatmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    int ok = write(fd, all, (size_t)m) == m && fsync(fd) == 0;
    ok = (close(fd) == 0) && ok;
    if (!ok || rename(d->metatmp, d->meta) != 0) { unlink(d->metatmp); return -1; }
    fsync_dir(d->c->dir);
    d->durable = d->committed;
    return 0;
}
/* 0 ok, 1 absent, -1 damaged or for something else (why filled) */
static int meta_read(Dl *d, uint64_t *committed, char *etag, size_t etagcap, char *why, size_t whycap) {
    int fd = open(d->meta, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? 1 : (setmsg(why, whycap, "cannot open the metadata (%s)", strerror(errno)), -1);
    char buf[1100]; ssize_t n = read(fd, buf, sizeof buf - 1); close(fd);
    if (n <= 0) { setmsg(why, whycap, "the metadata file is empty"); return -1; }
    buf[n] = 0;
    char *ck = strstr(buf, "check=");
    if (!ck || ck == buf || ck[-1] != '\n') { setmsg(why, whycap, "the metadata has no checksum line"); return -1; }
    unsigned long long want = 0; char tail = 0;
    if (sscanf(ck, "check=%16llx%c", &want, &tail) != 2 || tail != '\n' || fnv(buf, (size_t)(ck - buf)) != want) { setmsg(why, whycap, "the metadata checksum does not match (file damaged)"); return -1; }
    *ck = 0;
    if (strncmp(buf, "HENGE-DL 1\n", 11)) { setmsg(why, whycap, "the metadata is of another version"); return -1; }
    char *url = NULL, *et = NULL; unsigned long long size = 0, com = 0; int have_size = 0, have_com = 0;
    for (char *l = buf + 11; *l;) {
        char *e = strchr(l, '\n'); if (!e) break; *e = 0;
        if (!strncmp(l, "url=", 4)) url = l + 4;
        else if (!strncmp(l, "etag=", 5)) et = l + 5;
        else if (!strncmp(l, "size=", 5)) { size = strtoull(l + 5, NULL, 10); have_size = 1; }
        else if (!strncmp(l, "committed=", 10)) { com = strtoull(l + 10, NULL, 10); have_com = 1; }
        l = e + 1;
    }
    if (!url || !et || !have_size || !have_com) { setmsg(why, whycap, "the metadata is incomplete"); return -1; }
    if (strcmp(url, d->c->url)) { setmsg(why, whycap, "the metadata is for another URL"); return -1; }
    if (size != d->c->expect_size) { setmsg(why, whycap, "the metadata is for another file size (%llu)", size); return -1; }
    if (com > size) { setmsg(why, whycap, "the metadata says more bytes than the file has"); return -1; }
    if (!et[0] || !strncmp(et, "W/", 2)) { setmsg(why, whycap, "the metadata holds no strong ETag"); return -1; }
    if (strlen(et) >= etagcap) { setmsg(why, whycap, "the metadata ETag is too long"); return -1; }
    snprintf(etag, etagcap, "%s", et); *committed = com;
    return 0;
}

static int commit(Dl *d) {   /* data first, then the metadata */
    if (fsync(d->fd) != 0) { d->meta_broken = 1; return -1; }
    if (meta_write(d) != 0) { d->meta_broken = 1; return -1; }
    return 0;
}

static int hash_prefix(Dl *d, int fd, uint64_t n) {   /* hashes bytes [0, n) of fd into d->sha */
    static unsigned char buf[1 << 20];
    uint64_t off = 0, last = 0;
    while (off < n) {
        size_t want = n - off > sizeof buf ? sizeof buf : (size_t)(n - off);
        ssize_t r = pread(fd, buf, want, (off_t)off);
        if (r <= 0) return -1;
        mbedtls_sha256_update(&d->sha, buf, (size_t)r);
        off += (uint64_t)r;
        if (off - last >= (512ULL << 20)) { lg(d, "re-reading %llu of %llu bytes", LLU(off), LLU(n)); last = off; }
    }
    return 0;
}
static void to_hex(const unsigned char *h, char *o) { static const char x[] = "0123456789abcdef"; for (int i = 0; i < 32; i++) { o[2 * i] = x[h[i] >> 4]; o[2 * i + 1] = x[h[i] & 15]; } o[64] = 0; }

/* ---- one transfer ---- */
typedef struct {
    Dl *d;
    uint64_t start;
    int status, headers_done, refused, transient, identity, weak;
    char reason[240];
    uint64_t cr_start, cr_end, cr_total; int have_cr;
    char r_etag[160];
    uint64_t body_got, wire;
    int paused, cancelled, io_error, io_errno;
} Rq;

static void refuse(Rq *r, int transient, const char *fmt, ...) {
    if (r->refused) return;
    r->refused = 1; r->transient = transient;
    va_list ap; va_start(ap, fmt); vsnprintf(r->reason, sizeof r->reason, fmt, ap); va_end(ap);
}
static void trim(char *s) { size_t n = strlen(s); while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ')) s[--n] = 0; }

static int finish_headers(Rq *r) {
    Dl *d = r->d;
    r->headers_done = 1;
    if (r->status != 206) {
        if (r->status == 200) refuse(r, 0, "server answered 200, not 206: Range was ignored or the file changed (If-Range failed)");
        else if (r->status == 408 || r->status == 429 || r->status >= 500) refuse(r, 1, "server answered %d (try again later)", r->status);
        else refuse(r, 0, "server answered %d", r->status);
        return -1;
    }
    if (!r->identity) { refuse(r, 0, "reply is content-encoded"); return -1; }
    if (!r->have_cr) { refuse(r, 0, "206 without a usable Content-Range"); return -1; }
    if (r->cr_total != d->c->expect_size) { refuse(r, 0, "the server's file size is %llu, expected %llu", LLU(r->cr_total), LLU(d->c->expect_size)); return -1; }
    if (r->cr_start != r->start) { refuse(r, 0, "Content-Range starts at %llu, not at the requested %llu", LLU(r->cr_start), LLU(r->start)); return -1; }
    if (r->cr_end != d->c->expect_size - 1) { refuse(r, 0, "Content-Range ends at %llu, not at the end of the file", LLU(r->cr_end)); return -1; }
    if (d->etag[0]) {
        if (strcmp(r->r_etag, d->etag)) { refuse(r, 0, "ETag changed: the file on the server is not the one we started with"); return -1; }
    } else {
        if (r->weak) { refuse(r, 0, "only a weak ETag: refusing (a strong one is required)"); return -1; }
        if (!r->r_etag[0]) { refuse(r, 0, "no ETag: refusing (a strong one is required)"); return -1; }
        snprintf(d->etag, sizeof d->etag, "%s", r->r_etag);
        if (meta_write(d) != 0) { r->io_error = 1; r->io_errno = errno; refuse(r, 1, "cannot write the metadata"); return -1; }
    }
    return 0;
}
static size_t hdr_cb(char *p, size_t s, size_t n, void *ud) {
    Rq *r = ud; size_t tot = s * n;
    char line[400];
    if (r->refused) return tot;
    size_t m = tot < sizeof line - 1 ? tot : sizeof line - 1;
    memcpy(line, p, m); line[m] = 0; trim(line);
    if (!strncmp(line, "HTTP/", 5)) {
        const char *sp = strchr(line, ' ');
        r->status = sp ? atoi(sp + 1) : 0;
        r->have_cr = 0; r->r_etag[0] = 0; r->weak = 0; r->identity = 1; r->headers_done = 0;
    } else if (!line[0]) {
        if (!r->headers_done && r->status >= 200 && r->status / 100 != 3) finish_headers(r);
    } else if (!strncasecmp(line, "Content-Range:", 14)) {
        unsigned long long a, b, t;
        if (sscanf(line + 14, " bytes %llu-%llu/%llu", &a, &b, &t) == 3 && a <= b && b < t) { r->cr_start = a; r->cr_end = b; r->cr_total = t; r->have_cr = 1; }
    } else if (!strncasecmp(line, "ETag:", 5)) {
        const char *v = line + 5; while (*v == ' ') v++;
        if (!strncmp(v, "W/", 2)) r->weak = 1;
        else if (strlen(v) < sizeof r->r_etag) snprintf(r->r_etag, sizeof r->r_etag, "%s", v);   /* an ETag too long to store counts as none */
    } else if (!strncasecmp(line, "Content-Encoding:", 17)) {
        const char *v = line + 17; while (*v == ' ') v++;
        if (strcasecmp(v, "identity")) r->identity = 0;
    }
    return tot;
}
static size_t body_cb(char *p, size_t s, size_t n, void *ud) {
    Rq *r = ud; Dl *d = r->d; size_t tot = s * n;
    r->wire += tot;
    if (!r->headers_done) finish_headers(r);
    if (r->refused) return 0;      /* nothing of a refused reply is written */
    if (d->c->cancel && *d->c->cancel) { r->cancelled = 1; return 0; }
    if (r->body_got + tot > d->c->expect_size - r->start) { refuse(r, 0, "more body than the Content-Range announced"); return 0; }
    size_t take = tot;
    if (d->c->stop_after) { uint64_t room = d->c->stop_after > d->committed ? d->c->stop_after - d->committed : 0; if (take > room) take = (size_t)room; }
    size_t off = 0;
    while (off < take) {
        ssize_t w = write(d->fd, p + off, take - off);
        if (w < 0) { if (errno == EINTR) continue; r->io_error = 1; r->io_errno = errno; return 0; }
        off += (size_t)w;
    }
    mbedtls_sha256_update(&d->sha, (const unsigned char *)p, take);
    d->committed += take; r->body_got += take;
    if (d->c->stop_after && d->committed >= d->c->stop_after && d->committed < d->c->expect_size) { r->paused = 1; return 0; }
    if (d->committed - d->durable >= d->commit_every) { if (commit(d) != 0) { r->io_error = 1; r->io_errno = errno; return 0; } }
    long long t = now_ns();
    if (t - d->last_prog_ns >= 1000000000LL) { d->last_prog_ns = t; lg(d, "PROGRESS %llu %llu", LLU(d->committed), LLU(d->c->expect_size)); }
    return tot;
}
static int xfer_cb(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t e) {
    (void)a; (void)b; (void)c; (void)e;
    Rq *r = ud;
    if (r->d->c->cancel && *r->d->c->cancel) { r->cancelled = 1; return 1; }
    return 0;
}

static int lock_dir(Dl *d, char *msg, size_t cap) {
    d->lockfd = open(d->lockp, O_RDWR | O_CREAT, 0600);
    if (d->lockfd < 0) { setmsg(msg, cap, "cannot open the lock file (%s)", strerror(errno)); return DL_REFUSED; }
    if (flock(d->lockfd, LOCK_EX | LOCK_NB) != 0) { setmsg(msg, cap, "another download of this file is running"); close(d->lockfd); d->lockfd = -1; return DL_REFUSED; }
    return 0;
}
static int init_paths(Dl *d, const DlCfg *c, char *msg, size_t cap) {
    memset(d, 0, sizeof *d);
    d->c = c; d->lockfd = -1; d->fd = -1;
    d->commit_every = c->commit_every ? c->commit_every : (16ULL << 20);
    d->reserve = c->reserve;
    if (!c->url || !c->dir || !c->name || !c->name[0] || strchr(c->name, '/') || !strcmp(c->name, ".") || !strcmp(c->name, "..")) { setmsg(msg, cap, "bad arguments"); return DL_REFUSED; }
    if (strlen(c->url) > 400) { setmsg(msg, cap, "URL too long"); return DL_REFUSED; }
    if (!url_ok(c->url)) { setmsg(msg, cap, "refusing a URL that is not https"); return DL_REFUSED; }
    if (!c->expect_size) { setmsg(msg, cap, "no expected size"); return DL_REFUSED; }
    if (!is_hex64(c->expect_sha256)) { setmsg(msg, cap, "the expected SHA-256 must be 64 hex digits"); return DL_REFUSED; }
    struct stat st;
    if (stat(c->dir, &st) != 0 || !S_ISDIR(st.st_mode)) { setmsg(msg, cap, "the folder does not exist"); return DL_REFUSED; }
    if (snprintf(d->fin, PATHCAP, "%s/%s", c->dir, c->name) >= PATHCAP || snprintf(d->part, PATHCAP, "%s/%s.part", c->dir, c->name) >= PATHCAP ||
        snprintf(d->meta, PATHCAP, "%s/%s.meta", c->dir, c->name) >= PATHCAP || snprintf(d->metatmp, PATHCAP, "%s/%s.meta.tmp", c->dir, c->name) >= PATHCAP ||
        snprintf(d->lockp, PATHCAP, "%s/%s.lock", c->dir, c->name) >= PATHCAP) { setmsg(msg, cap, "path too long"); return DL_REFUSED; }
    return 0;
}
static void cleanup(Dl *d) {
    if (d->fd >= 0) { close(d->fd); d->fd = -1; }
    if (d->lockfd >= 0) { close(d->lockfd); d->lockfd = -1; }
}

static int finalize(Dl *d, char *msg, size_t cap) {
    unsigned char dg[32]; char hex[65];
    mbedtls_sha256_finish(&d->sha, dg); to_hex(dg, hex);
    lg(d, "SHA256 computed %s", hex);
    lg(d, "SHA256 expected %s", d->c->expect_sha256);
    if (strcasecmp(hex, d->c->expect_sha256)) {
        setmsg(msg, cap, "the file is complete but its SHA-256 differs from the expected value: NOT installed, the .part file is kept (the file may be damaged, or the expected value may be wrong)");
        return DL_HASH_MISMATCH;
    }
    if (fsync(d->fd) != 0) { setmsg(msg, cap, "fsync failed (%s)", strerror(errno)); return DL_INTERRUPTED; }
    close(d->fd); d->fd = -1;
    if (rename(d->part, d->fin) != 0) { setmsg(msg, cap, "cannot move the file to its final name (%s)", strerror(errno)); return DL_INTERRUPTED; }
    fsync_dir(d->c->dir);
    unlink(d->meta);
    setmsg(msg, cap, "complete: %llu bytes, SHA-256 verified, installed as %s", LLU(d->c->expect_size), d->c->name);
    return DL_DONE;
}

int ipsw_dl_run(const DlCfg *c, char *msg, size_t cap) {
    Dl dl, *d = &dl; int rc;
    if (msg && cap) msg[0] = 0;
    if ((rc = init_paths(d, c, msg, cap))) return rc;
    if ((rc = lock_dir(d, msg, cap))) return rc;
    mbedtls_sha256_init(&d->sha);
    char why[256];
    struct stat st;

    /* 1. the final file already exists: verify it, no network */
    if (stat(d->fin, &st) == 0) {
        if ((uint64_t)st.st_size != c->expect_size) { setmsg(msg, cap, "a file with the final name exists but has %llu bytes, not %llu: not touching it", LLU(st.st_size), LLU(c->expect_size)); rc = DL_STATE; goto out; }
        int fd = open(d->fin, O_RDONLY);
        mbedtls_sha256_starts(&d->sha, 0);
        if (fd < 0 || hash_prefix(d, fd, c->expect_size) != 0) { if (fd >= 0) close(fd); setmsg(msg, cap, "cannot read the existing final file"); rc = DL_STATE; goto out; }
        close(fd);
        unsigned char dg[32]; char hex[65]; mbedtls_sha256_finish(&d->sha, dg); to_hex(dg, hex);
        lg(d, "SHA256 computed %s", hex); lg(d, "SHA256 expected %s", c->expect_sha256);
        if (strcasecmp(hex, c->expect_sha256)) { setmsg(msg, cap, "the final file exists but its SHA-256 differs from the expected value"); rc = DL_HASH_MISMATCH; goto out; }
        setmsg(msg, cap, "already complete: SHA-256 verified"); rc = DL_DONE; goto out;
    }

    /* 2. the state on disk */
    uint64_t committed = 0; int have_meta = 0;
    char etag[160] = "";
    int mr = meta_read(d, &committed, etag, sizeof etag, why, sizeof why);
    if (mr < 0) { setmsg(msg, cap, "%s: not resuming automatically (delete the partial download to start over)", why); rc = DL_STATE; goto out; }
    have_meta = (mr == 0);
    /* free space, before anything is created or sent (the committed bytes are already on disk) */
    {
        uint64_t target = c->stop_after && c->stop_after < c->expect_size ? c->stop_after : c->expect_size;
        uint64_t cm = have_meta ? committed : 0;
        uint64_t need = (target > cm ? target - cm : 0) + (target > cm ? d->reserve : 0);
        struct statvfs sv;
        if (statvfs(c->dir, &sv) != 0) { setmsg(msg, cap, "cannot read the free space (%s)", strerror(errno)); rc = DL_REFUSED; goto out; }
        uint64_t avail = (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
        if (c->avail_cap && c->avail_cap < avail) avail = c->avail_cap;
        if (need > avail) { setmsg(msg, cap, "not enough free space: %llu MiB needed (remaining bytes plus a reserve), %llu MiB free", LLU(need >> 20), LLU(avail >> 20)); rc = DL_REFUSED; goto out; }
    }

    int pfd = open(d->part, O_RDWR);
    int part_exists = pfd >= 0;
    if (!part_exists && errno != ENOENT) { setmsg(msg, cap, "cannot open the .part file (%s)", strerror(errno)); rc = DL_STATE; goto out; }
    uint64_t psize = 0;
    if (part_exists) { struct stat ps; if (fstat(pfd, &ps) != 0) { close(pfd); setmsg(msg, cap, "cannot stat the .part file"); rc = DL_STATE; goto out; } psize = (uint64_t)ps.st_size; }
    if (part_exists && !have_meta && psize > 0) { close(pfd); setmsg(msg, cap, "a .part file without metadata: not resuming automatically (delete the partial download to start over)"); rc = DL_STATE; goto out; }
    if (!part_exists && have_meta && committed > 0) { setmsg(msg, cap, "metadata without its .part file: not resuming automatically (delete the partial download to start over)"); rc = DL_STATE; goto out; }
    if (part_exists && have_meta && psize < committed) { close(pfd); setmsg(msg, cap, "the .part file (%llu bytes) is shorter than the %llu bytes the metadata committed: not resuming automatically", LLU(psize), LLU(committed)); rc = DL_STATE; goto out; }
    if (!part_exists) { pfd = open(d->part, O_RDWR | O_CREAT, 0600); if (pfd < 0) { setmsg(msg, cap, "cannot create the .part file (%s)", strerror(errno)); rc = DL_STATE; goto out; } }
    d->fd = pfd;
    if (!have_meta) committed = 0;
    if (psize > committed) { lg(d, "cutting %llu uncommitted bytes off the .part file", LLU(psize - committed)); if (ftruncate(d->fd, (off_t)committed) != 0) { setmsg(msg, cap, "cannot truncate (%s)", strerror(errno)); rc = DL_STATE; goto out; } }
    d->committed = committed; d->durable = committed;
    if (have_meta) snprintf(d->etag, sizeof d->etag, "%s", etag);
    lg(d, "STATE committed=%llu size=%llu", LLU(committed), LLU(c->expect_size));

    /* 4. already at the pause point: nothing to send */
    if (c->stop_after && committed >= c->stop_after && committed < c->expect_size) {
        setmsg(msg, cap, "already at the pause point (%llu bytes committed)", LLU(committed)); rc = DL_PAUSED; goto out;
    }

    /* 5. read the committed prefix again so the final SHA-256 covers everything */
    mbedtls_sha256_starts(&d->sha, 0);
    if (committed) { lg(d, "re-reading %llu committed bytes", LLU(committed)); if (hash_prefix(d, d->fd, committed) != 0) { setmsg(msg, cap, "the .part file cannot be read back: not resuming automatically"); rc = DL_STATE; goto out; } }
    if (committed == c->expect_size) { rc = finalize(d, msg, cap); goto out; }
    if (lseek(d->fd, (off_t)committed, SEEK_SET) < 0) { setmsg(msg, cap, "cannot seek (%s)", strerror(errno)); rc = DL_STATE; goto out; }

    /* 6. the transfer */
    Rq r; memset(&r, 0, sizeof r);
    r.d = d; r.start = committed; r.identity = 1;
    CURL *h = curl_easy_init();
    if (!h) { setmsg(msg, cap, "curl init failed"); rc = DL_INTERRUPTED; goto out; }
    char rng[64], ifr[220], errbuf[CURL_ERROR_SIZE]; errbuf[0] = 0;
    snprintf(rng, sizeof rng, "Range: bytes=%llu-", LLU(committed));
    struct curl_slist *hd = curl_slist_append(NULL, rng);
    hd = curl_slist_append(hd, "Accept-Encoding: identity");
    if (d->etag[0]) { snprintf(ifr, sizeof ifr, "If-Range: %s", d->etag); hd = curl_slist_append(hd, ifr); }
    lg(d, "REQUEST %s%s", rng + 7, d->etag[0] ? " (If-Range with the stored strong ETag)" : " (first request, ETag not yet known)");
    curl_easy_setopt(h, CURLOPT_URL, c->url);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hd);
    curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, (curl_write_callback)hdr_cb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &r);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, (curl_write_callback)body_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &r);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, (curl_xferinfo_callback)xfer_cb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &r);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, c->connect_timeout_s ? c->connect_timeout_s : 20L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 0L);   /* no limit on the whole transfer; the stand-in's idle limit (60 s) applies */
    curl_easy_setopt(h, CURLOPT_USERAGENT, "Henge-ipsw-dl/1");
    CURLcode crc = curl_easy_perform(h);
    curl_slist_free_all(hd); curl_easy_cleanup(h);
    if (!r.refused && crc == CURLE_OK && !r.headers_done) finish_headers(&r);
    if (d->etag[0]) commit(d);      /* leave a consistent state behind, whatever happened */
    lg(d, "PROGRESS %llu %llu", LLU(d->committed), LLU(c->expect_size));

    if (r.io_error) { setmsg(msg, cap, "disk write failed (%s): the partial download is kept", strerror(r.io_errno)); rc = DL_INTERRUPTED; goto out; }
    if (d->meta_broken) { setmsg(msg, cap, "the state could not be saved to disk: the partial download is kept"); rc = DL_INTERRUPTED; goto out; }
    if (r.refused) { setmsg(msg, cap, "%s", r.reason); rc = r.transient ? DL_INTERRUPTED : DL_SERVER; goto out; }
    if (r.cancelled) { setmsg(msg, cap, "stopped on request at %llu bytes; run again to continue", LLU(d->committed)); rc = DL_INTERRUPTED; goto out; }
    if (r.paused) { setmsg(msg, cap, "paused at %llu bytes as planned; run again to continue", LLU(d->committed)); rc = DL_PAUSED; goto out; }
    if (crc != CURLE_OK) { setmsg(msg, cap, "transfer interrupted at %llu bytes: %s", LLU(d->committed), errbuf[0] ? errbuf : "network error"); rc = DL_INTERRUPTED; goto out; }
    if (d->committed != c->expect_size) { setmsg(msg, cap, "the server closed the connection at %llu of %llu bytes", LLU(d->committed), LLU(c->expect_size)); rc = DL_INTERRUPTED; goto out; }
    rc = finalize(d, msg, cap);
out:
    mbedtls_sha256_free(&d->sha);
    cleanup(d);
    return rc;
}

int ipsw_dl_status(const DlCfg *c, FILE *out) {
    Dl dl, *d = &dl; char msg[200];
    int rc = init_paths(d, c, msg, sizeof msg);
    if (rc) { fprintf(out, "dl: STATUS error: %s\n", msg); return rc; }
    struct stat st; long long fin = stat(d->fin, &st) == 0 ? (long long)st.st_size : -1;
    long long part = stat(d->part, &st) == 0 ? (long long)st.st_size : -1;
    uint64_t com = 0; char et[160], why[200];
    int mr = meta_read(d, &com, et, sizeof et, why, sizeof why);
    fprintf(out, "dl: STATUS total=%llu final=%lld part=%lld committed=%lld meta=%s%s%s\n", LLU(c->expect_size), fin, part, mr == 0 ? (long long)com : -1LL,
            mr == 0 ? "ok" : mr == 1 ? "none" : "damaged", mr < 0 ? ": " : "", mr < 0 ? why : "");
    return 0;
}

int ipsw_dl_delete_partial(const DlCfg *c, char *msg, size_t cap) {
    Dl dl, *d = &dl; int rc;
    if ((rc = init_paths(d, c, msg, cap))) return rc;
    if ((rc = lock_dir(d, msg, cap))) return rc;
    int a = unlink(d->part), b = unlink(d->meta); unlink(d->metatmp);
    fsync_dir(c->dir);
    cleanup(d);
    setmsg(msg, cap, "partial download removed (part %s, metadata %s); a finished file is never removed here", a == 0 ? "deleted" : "absent", b == 0 ? "deleted" : "absent");
    return 0;
}
