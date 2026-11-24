#ifndef HENGEVM_CLONE_DIR_H
#define HENGEVM_CLONE_DIR_H
/* The checks of host/ui_runner_vm_clone.c that decide whether anything may be written: whether a folder is a finished, plain clone OF THIS APP'S VM FOLDER, tied to this machine's ECID, and the ECID read.
   Header-only so that tests/test_clone_dir.c runs the same code on a Linux machine.
   hv_clone_open() checks and OPENS in one step: the files it checked are kept open (as file descriptors of this process) and the machine is given /proc/self/fd/N paths, so a file swapped for another
   after the check cannot be what is written. The clone's folder is locked (flock, non-blocking) so a second runner on the same clone is refused. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- SHA-256 (small, for the ECID tag; the same algorithm the restore preflight and G7Clone.ecidTag use) ---- */
static inline uint32_t hv_cd_ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline void hv_cd_sha256(const uint8_t *msg, size_t len, uint8_t out[32]) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint8_t blk[128]; size_t i, full = len / 64;
    for (size_t b = 0; b < full + 2; b++) {
        const uint8_t *p;
        if (b < full) p = msg + b * 64;
        else {
            size_t rem = len - full * 64; memset(blk, 0, sizeof blk); memcpy(blk, msg + full * 64, rem); blk[rem] = 0x80;
            size_t total = rem < 56 ? 64 : 128; uint64_t bits = (uint64_t)len * 8;
            for (i = 0; i < 8; i++) blk[total - 1 - i] = (uint8_t)(bits >> (8 * i));
            if (b == full + 1 && total == 64) break;
            p = blk + (b - full) * 64;
        }
        uint32_t w[64], a, bb, c, d, e, f, g, hh;
        for (i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
        for (i = 16; i < 64; i++) { uint32_t s0 = hv_cd_ror(w[i-15],7) ^ hv_cd_ror(w[i-15],18) ^ (w[i-15] >> 3), s1 = hv_cd_ror(w[i-2],17) ^ hv_cd_ror(w[i-2],19) ^ (w[i-2] >> 10); w[i] = w[i-16] + s0 + w[i-7] + s1; }
        a=h[0]; bb=h[1]; c=h[2]; d=h[3]; e=h[4]; f=h[5]; g=h[6]; hh=h[7];
        for (i = 0; i < 64; i++) {
            uint32_t t1 = hh + (hv_cd_ror(e,6) ^ hv_cd_ror(e,11) ^ hv_cd_ror(e,25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (hv_cd_ror(a,2) ^ hv_cd_ror(a,13) ^ hv_cd_ror(a,22)) + ((a & bb) ^ (a & c) ^ (bb & c));
            hh=g; g=f; f=e; e=d+t1; d=c; c=bb; bb=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=bb; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    for (i = 0; i < 8; i++) { out[4*i] = (uint8_t)(h[i] >> 24); out[4*i+1] = (uint8_t)(h[i] >> 16); out[4*i+2] = (uint8_t)(h[i] >> 8); out[4*i+3] = (uint8_t)h[i]; }
}

/* The tag tied to an ECID: first 8 bytes (16 hex digits) of SHA-256("henge-g7-ecid" NUL + the ECID as 8 big-endian bytes). */
static inline void hv_clone_ecid_tag(uint64_t ecid, char out[17]) {
    uint8_t in[14 + 8], d[32]; memcpy(in, "henge-g7-ecid", 13); in[13] = 0;
    for (int i = 0; i < 8; i++) in[14 + i] = (uint8_t)(ecid >> (56 - 8 * i));
    hv_cd_sha256(in, sizeof in, d);
    for (int i = 0; i < 8; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

typedef struct { int dirfd, disk_fd, aux_fd; char disk[48], aux[48], real[PATH_MAX]; int has_initial; } HvClone;

static inline int hv_cd_read_all(int fd, char *buf, size_t cap) {   /* whole file or -1 (empty, or does not fit with room to spare) */
    size_t n = 0; for (;;) { ssize_t r = read(fd, buf + n, cap - 1 - n); if (r < 0) return -1; if (r == 0) break; n += (size_t)r; if (n >= cap - 1) return -1; }
    if (n == 0) return -1;
    buf[n] = 0;
    return (int)n;
}
/* A line "key=value" at line start; copies the value (up to the line end) into out; 0 when absent. */
static inline int hv_cd_line(const char *text, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    for (const char *p = text; p && *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == '=') { const char *v = p + kl + 1; size_t l = strcspn(v, "\n"); if (l >= cap) return 0; memcpy(out, v, l); out[l] = 0; return 1; }
        p = strchr(p, '\n'); if (p) p++;
    }
    return 0;
}
static inline int hv_cd_u64(const char *s, unsigned long long *v) {
    if (!*s || strlen(s) > 20) return 0;
    for (const char *q = s; *q; q++) { if (*q < '0' || *q > '9') return 0; }
    *v = strtoull(s, NULL, 10);
    return 1;
}

/* Checks the clone folder `dir` and opens its members. dir must be <ecid_dir>/clones/clone-NNN (both given as plain absolute paths); the folder holds only the known files; the completion marker has exactly
   the lines the app writes, with the disk size of the file and the tag of THIS ECID; g7_state.txt carries the same tag; the disk and AUX are plain, single-link files that are not the original pair's files.
   0 and a filled HvClone (descriptors open: close them with hv_clone_close), or -1 with a reason (nothing left open). */
static inline void hv_clone_close(HvClone *c) { if (c->disk_fd >= 0) close(c->disk_fd); if (c->aux_fd >= 0) close(c->aux_fd); if (c->dirfd >= 0) close(c->dirfd); c->disk_fd = c->aux_fd = c->dirfd = -1; }

static inline int hv_clone_open(const char *dir, const char *ecid_dir, uint64_t ecid, HvClone *c, char *why, size_t wcap) {
#define BAD(...) do { snprintf(why, wcap, __VA_ARGS__); hv_clone_close(c); return -1; } while (0)
    c->dirfd = c->disk_fd = c->aux_fd = -1; c->has_initial = 0;
    char real[PATH_MAX], ereal[PATH_MAX], tag[17];
    if (ecid == 0) BAD("no machine identity (ECID) to tie the clone to");
    hv_clone_ecid_tag(ecid, tag);
    if (!dir || dir[0] != '/') BAD("--clone-dir must be an absolute path");
    if (!ecid_dir || ecid_dir[0] != '/' || !realpath(ecid_dir, ereal) || strcmp(ereal, ecid_dir)) BAD("--ecid-dir is not a plain absolute path");
    if (!realpath(dir, real) || strcmp(real, dir)) BAD("--clone-dir is not a plain path (a link or a relative part)");
    size_t el = strlen(ereal);
    if (strncmp(real, ereal, el) || strncmp(real + el, "/clones/clone-", 14) || strlen(real) != el + 14 + 3 || !isdigit((unsigned char)real[el + 14]) || !isdigit((unsigned char)real[el + 15]) || !isdigit((unsigned char)real[el + 16]))
        BAD("the folder is not <vm folder>/clones/clone-NNN of the folder that holds the machine identity");
    c->dirfd = open(real, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (c->dirfd < 0) BAD("cannot open the clone folder: %s", strerror(errno));
    if (flock(c->dirfd, LOCK_EX | LOCK_NB) != 0) BAD("the clone is in use by another runner (or cannot be locked)");
    /* only the known files, nothing else (a .partial file, a stray link, an unknown name all refuse) */
    int d2 = dup(c->dirfd); DIR *dd = d2 >= 0 ? fdopendir(d2) : NULL;
    if (!dd) { if (d2 >= 0) close(d2); BAD("cannot list the clone folder"); }
    struct dirent *de;
    while ((de = readdir(dd))) {
        const char *n = de->d_name;
        if (!strcmp(n, ".") || !strcmp(n, "..") || !strcmp(n, "macos_disk.img") || !strcmp(n, "macos_aux.img") || !strcmp(n, "g7_state.txt") || !strcmp(n, "clone_state.txt")) continue;
        if (!strcmp(n, "macos_aux.initial")) { c->has_initial = 1; continue; }
        closedir(dd); BAD("the clone folder holds a file that does not belong to a clone");
    }
    closedir(dd);
    struct stat sd, sa, ss, sm;
    c->disk_fd = openat(c->dirfd, "macos_disk.img", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    c->aux_fd = openat(c->dirfd, "macos_aux.img", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (c->disk_fd < 0 || c->aux_fd < 0) BAD("macos_disk.img or macos_aux.img is missing or is a link");
    if (fstat(c->disk_fd, &sd) || !S_ISREG(sd.st_mode) || sd.st_nlink != 1) BAD("macos_disk.img is not a plain file or has another link");
    if (fstat(c->aux_fd, &sa) || !S_ISREG(sa.st_mode) || sa.st_nlink != 1) BAD("macos_aux.img is not a plain file or has another link");
    if (c->has_initial) { int fi = openat(c->dirfd, "macos_aux.initial", O_RDONLY | O_NOFOLLOW | O_CLOEXEC); struct stat si; int bad = fi < 0 || fstat(fi, &si) || !S_ISREG(si.st_mode) || si.st_nlink != 1; if (fi >= 0) close(fi); if (bad) BAD("macos_aux.initial is not a plain single-link file"); }
    /* not the original pair: a different inode than the files of the VM folder (a hard link is excluded by the link count above; this also excludes anything else sharing an inode) */
    char op[PATH_MAX + 32]; struct stat so;
    snprintf(op, sizeof op, "%s/macos_disk.img", ereal); if (stat(op, &so) == 0 && so.st_dev == sd.st_dev && so.st_ino == sd.st_ino) BAD("the clone's disk is the original disk");
    snprintf(op, sizeof op, "%s/macos_aux.img", ereal); if (stat(op, &so) == 0 && so.st_dev == sa.st_dev && so.st_ino == sa.st_ino) BAD("the clone's AUX is the original AUX");
    /* g7_state.txt: the tag of this ECID */
    char sf[4096], tv[64]; int fs = openat(c->dirfd, "g7_state.txt", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fs < 0 || fstat(fs, &ss) || !S_ISREG(ss.st_mode) || hv_cd_read_all(fs, sf, sizeof sf) < 0) { if (fs >= 0) close(fs); BAD("g7_state.txt is missing, not a plain file or unreadable"); }
    close(fs);
    if (!hv_cd_line(sf, "ecid_tag", tv, sizeof tv)) BAD("the clone is not tied to an ECID (no ecid_tag line)");
    if (strcmp(tv, tag)) BAD("the clone belongs to another machine identity (ECID tag differs)");
    /* the completion marker: exactly six lines */
    char mk[600]; int fm = openat(c->dirfd, "clone_state.txt", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fm < 0) BAD("the clone has no completion marker (clone_state.txt): it is not a finished clone");
    if (fstat(fm, &sm) || !S_ISREG(sm.st_mode) || hv_cd_read_all(fm, mk, sizeof mk) < 0) { close(fm); BAD("the completion marker is unreadable or too large"); }
    close(fm);
    int nl = 0; for (const char *q = mk; *q; q++) if (*q == '\n') nl++;
    size_t mlen = strlen(mk);
    if (nl != 6 || mk[mlen - 1] != '\n') BAD("the completion marker does not have the expected six lines");
    static const char *keys[6] = {"state", "source_state", "logical_disk_bytes", "allocated_bytes", "members", "ecid_tag"};
    const char *p = mk; char val[64]; unsigned long long num;
    for (int i = 0; i < 6; i++) {
        size_t kl = strlen(keys[i]); size_t ll = strcspn(p, "\n");
        if (strncmp(p, keys[i], kl) || p[kl] != '=' || ll - kl - 1 >= sizeof val) BAD("the completion marker line %d is not %s=", i + 1, keys[i]);
        memcpy(val, p + kl + 1, ll - kl - 1); val[ll - kl - 1] = 0;
        if (i == 0 && strcmp(val, "clone_complete")) BAD("the marker does not say clone_complete");
        if (i == 1 && strcmp(val, "in_progress") && strcmp(val, "done")) BAD("the marker's source state is not known");
        if (i == 2 && (!hv_cd_u64(val, &num) || num != (unsigned long long)sd.st_size)) BAD("the marker's disk size does not match macos_disk.img");
        if (i == 3 && !hv_cd_u64(val, &num)) BAD("the marker's allocated size is not a number");
        if (i == 4 && (!hv_cd_u64(val, &num) || num != (unsigned long long)(2 + (c->has_initial ? 1 : 0)))) BAD("the marker's member count does not match the files");
        if (i == 5 && strcmp(val, tag)) BAD("the marker belongs to another machine identity (ECID tag differs)");
        p += ll + 1;
    }
    snprintf(c->disk, sizeof c->disk, "/proc/self/fd/%d", c->disk_fd); snprintf(c->aux, sizeof c->aux, "/proc/self/fd/%d", c->aux_fd);
    snprintf(c->real, sizeof c->real, "%s", real);
    return 0;
#undef BAD
}

/* "ecid=0x<hex>" in <dir>/vm_ecid.txt, 2^40 <= value < 2^62. 0 on any problem (the value is never printed). */
static inline uint64_t hv_clone_read_ecid(const char *dir) {
    char path[PATH_MAX], t[64];
    if (snprintf(path, sizeof path, "%s/vm_ecid.txt", dir) >= (int)sizeof path) return 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); if (fd < 0) return 0;
    ssize_t n = read(fd, t, sizeof t - 1); close(fd); if (n < 8) return 0; t[n] = 0;
    while (n > 0 && (t[n - 1] == '\n' || t[n - 1] == '\r' || t[n - 1] == ' ')) t[--n] = 0;
    if (strncmp(t, "ecid=0x", 7) || n > 23) return 0;
    char *end; unsigned long long v = strtoull(t + 7, &end, 16);
    if (*end || v < (1ull << 40) || v >= (1ull << 62)) return 0;
    return v;
}

#endif
