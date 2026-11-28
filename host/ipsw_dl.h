/* Resumable download of one large file (the macOS IPSW) over HTTPS, with its integrity checked at the end. A tool layer on the curl stand-in
   (host/curl_mbedtls.c); it does not touch idevicerestore and it does not depend on any Android UI: it is an ordinary program, so an Activity,
   a foreground service or a shell can all run it.
   Rules, enforced here and tested in tools/ipswdl-test.sh against local servers:
   - https only (loopback http only in a build with HV_DL_ALLOW_HTTP_TEST, which no shipped build sets: tools/check-no-test-flags.sh).
   - the expected size and SHA-256 are inputs, never learned from the server. The file is installed under its final name only when the whole file has
     that size AND that SHA-256. On a mismatch the .part file and its metadata are kept and nothing is installed.
   - every reply must be 206 with a Content-Range that starts exactly at the byte we asked for, ends at the end of the file and names the expected total;
     a 200 (Range ignored, or If-Range failed) is aborted at its first body bytes without writing anything. A STRONG ETag is required, remembered in the
     metadata and sent as If-Range on every resume; a different ETag stops the download (the .part file is kept, the user decides).
   - state on disk: NAME.part (data), NAME.meta (url, size, ETag, committed bytes, checksum), NAME.lock (flock: one downloader at a time).
     Order of writes: data -> fsync -> metadata (temp file, fsync, rename, fsync of the directory). So after any crash .part is at least as long as
     "committed". Resume: longer -> the excess is cut off; equal -> as is; shorter or the metadata damaged or missing -> no automatic resume (exit 3).
     The committed prefix is read again and hashed before the transfer continues, so the final SHA-256 covers every byte of the file.
   - free space is checked before the first request (remaining bytes + a reserve); the caller may only lower the figure, never raise it.
   - stop_after: pause (not an error) once that many bytes are committed; .part and metadata stay and a later run continues.
   - reconnects: a dropped connection or a transient answer (408/429/5xx) is retried from the committed byte, with a doubling wait (bounded) and the same checks on
     every reply; the running hash is kept, nothing is read again. Progress resets the count of failures in a row; an untrustworthy answer (200, other range, ETag/size
     change, 4xx) is never retried. The count of failures in a row is reset only when at least DL_MIN_PROGRESS (1 MiB, an empirical value, not from a standard) has
     arrived SINCE THE LAST RESET, summed over attempts, so a server that hands out a few bytes per connection cannot keep the run alive. Without that progress
     the run ends with exit 5 after max_retries reconnects; in any case after max_total reconnects in one run.
   - parallel (parallel > 1, at least two segments left): the bytes from "committed" to the end (or to stop_after) are cut into segments of chunk_bytes. Up to
     "parallel" worker threads each fetch one segment with its own Range request (bytes=a-b; the same 206 / Content-Range / ETag / If-Range checks, plus: the range
     must end where we asked) and pwrite it straight into the single .part file at its place; there are no per-piece files. A segment that fails is retried from its
     own last byte, by any worker. Only segments up to 2 x parallel ahead of the first unfinished one are started, so a stalled segment cannot leave a huge hole.
     The SHA-256 stays incremental and in order: the coordinator (the calling thread) reads the contiguous prefix back from the file as it grows (page cache) and hashes
     it; "committed" in the metadata is that prefix only, written with the same order (data -> fsync -> metadata). So the files on disk, the metadata and the resume
     rules are the ones of the single stream: a .part file or metadata from either path resumes in either path, and bytes beyond the committed prefix are cut off.
     The first request when the ETag is not known yet is a one-byte probe, so every segment is checked against one strong ETag. A server limit (429 / 5xx / 408, or a
     connection that dies before delivering a byte) lowers the number of connections by one per such failure (never below 1); it is not raised again in that run and
     such a failure does not count as a failure in a row. Everything else (200, wrong range, ETag change, 4xx, disk error) ends the run as in the single stream.
     The log has "SPEED bytes_per_second conns=n allowed=m" once a second (also in the single stream) and "TIME wall=.. user=.. sys=.. (ms)" at the end.
   - cancel: *cancel != 0 ends the transfer (or a reconnect wait) at once; the state is flushed first; exit 8. */
#ifndef HV_IPSW_DL_H
#define HV_IPSW_DL_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define DL_MAX_PARALLEL 16
#define DL_CHUNK_DEFAULT (32ULL << 20)
#define DL_CHUNK_MIN (256ULL << 10)

typedef struct {
    const char *url;               /* https://... */
    const char *dir;               /* existing directory; all files live here */
    const char *name;              /* final file name (no slash) */
    uint64_t expect_size;          /* required, > 0 */
    const char *expect_sha256;     /* required, 64 hex digits */
    uint64_t stop_after;           /* 0 = download to the end; else pause at this many committed bytes */
    uint64_t avail_cap;            /* 0 = none; else free space is min(statvfs, avail_cap) */
    uint64_t reserve;              /* default 512 MiB kept free beyond the remaining bytes */
    uint64_t commit_every;         /* default 16 MiB: bytes between fsync + metadata */
    long connect_timeout_s;        /* default 20 */
    unsigned max_retries;          /* default 6: reconnects in a row WITHOUT progress before giving up (any progress resets the count); 0 = never reconnect */
    unsigned retry_base_ms;        /* default 2000: wait before a reconnect, doubled per failure in a row (at most x32, at most 60 s) */
    unsigned max_total;            /* default 50: reconnects in ONE run, however much progress there was (0 = the default); the next run starts the count again */
    unsigned parallel;             /* default 1 = one stream (the old path). 2..16 = that many connections, each fetching its own Range segment; see below */
    uint64_t chunk_bytes;          /* default 32 MiB: size of one segment in the parallel path (at least 256 KiB) */
    const volatile int *cancel;    /* optional */
    FILE *out;                     /* progress and log lines, prefixed "dl: " */
} DlCfg;

enum {
    DL_DONE = 0,            /* the file is complete, SHA-256 matches, installed under its final name */
    DL_REFUSED = 2,         /* nothing was sent: bad arguments, no space, another downloader, URL policy */
    DL_STATE = 3,           /* nothing was sent: the files on disk are inconsistent; no automatic resume */
    DL_SERVER = 4,          /* the server's answer cannot be trusted (200, wrong range, ETag/size changed, 4xx); .part kept */
    DL_INTERRUPTED = 5,     /* network trouble that outlasted the reconnects, a disk write error; .part and metadata are consistent; run again to resume */
    DL_HASH_MISMATCH = 6,   /* complete but SHA-256 differs; .part kept, nothing installed */
    DL_PAUSED = 7,          /* stop_after reached; run again to continue */
    DL_CANCELLED = 8        /* stopped on request (SIGTERM / cancel flag); state flushed, run again to continue */
};

void dl_cfg_default(DlCfg *c);
int ipsw_dl_run(const DlCfg *c, char *msg, size_t cap);        /* DL_* ; msg = the reason */
int ipsw_dl_status(const DlCfg *c, FILE *out);                 /* prints the state, no network; DL_* style code */
int ipsw_dl_delete_partial(const DlCfg *c, char *msg, size_t cap);   /* removes .part and .meta (never the final file); refuses while locked */
#endif
