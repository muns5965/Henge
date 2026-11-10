/* Reads single entries (BuildManifest.plist) out of a ZIP/ZIP64 file on an HTTPS server with HTTP Range requests, without downloading the file.
   An independent tool layer: it does not touch idevicerestore. libzip parses the archive through a seekable source whose reads become Range requests.
   Safety rules, all enforced here and tested in tools/remote-zip-test.sh against local servers:
   - https only (http only for 127.0.0.1/localhost in a build with HV_RZ_ALLOW_HTTP_TEST, which no shipped build sets: tools/check-no-test-flags.sh).
   - every reply must be 206 with a Content-Range that matches the request and the known file size; a 200 (range ignored, or If-Range failed) is aborted
     at its first body bytes. Bodies longer or shorter than the requested range are errors.
   - consistency: a STRONG ETag is remembered and sent as If-Range and compared on every reply; a weak ETag is never used. Without a strong ETag only the
     total size and Last-Modified are compared, which does not prove the content is the same (reported as consistency "weak"; require_strong_etag refuses it).
     Any change flushes the cache and makes every later read fail.
   - hard limits: requests, body bytes, declared entry size, entry count, file size; a read that cannot fit the byte budget fails before any request.
   - the entry is read at most its declared size + 1 byte; libzip checks the CRC-32 (a bad one is an error); encrypted entries are refused.
   - cancel: *limits.cancel != 0 ends the current transfer and every later one. */
#ifndef HV_REMOTE_ZIP_H
#define HV_REMOTE_ZIP_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct RemoteZip RemoteZip;
typedef struct {
    uint32_t block_size;          /* bytes per Range request, power of two >= 4096 (default 256 KiB) */
    uint32_t max_requests;        /* default 24 */
    uint64_t max_bytes;           /* body bytes accepted in total, default 4 MiB */
    uint64_t max_entry_bytes;     /* declared uncompressed size of the entry to read, default 4 MiB */
    uint32_t max_entries;         /* central directory entries, default 20000 */
    uint64_t max_file_size;       /* default 64 GiB */
    int require_strong_etag;      /* 1: refuse to open without a strong ETag */
    long request_timeout_s;       /* default 30 */
    long connect_timeout_s;       /* default 15 */
    const volatile int *cancel;   /* optional */
    FILE *trace;                  /* optional: one line per request (also those of rz_open) */
} RzLimits;
typedef struct {
    uint32_t requests;            /* requests started */
    uint64_t bytes;               /* body bytes accepted from 206 replies */
    uint64_t wire_bytes;          /* all body bytes received, including those of a refused reply */
    uint64_t size;                /* file size, 0 until known */
    int strong_etag;              /* 1 when the strong ETag is in use */
    int consistency_weak;         /* 1 when only size/Last-Modified guard against a changing file */
    int broken;                   /* the file changed or a reply was refused: nothing more is read */
} RzStats;

void rz_limits_default(RzLimits *l);
RemoteZip *rz_open(const char *url, const RzLimits *l, char *err, size_t errcap);   /* NULL + err on failure */
int rz_read_entry(RemoteZip *z, const char *name, uint8_t **out, size_t *len, char *err, size_t errcap); /* 0 ok */
int rz_read_at(RemoteZip *z, uint64_t off, void *buf, size_t n, char *err, size_t errcap);               /* raw ranged read, same checks */
const RzStats *rz_stats(const RemoteZip *z);
void rz_set_trace(RemoteZip *z, FILE *f);   /* one line per request: range, status, bytes */
void rz_close(RemoteZip *z);
#endif
