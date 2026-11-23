#define _GNU_SOURCE
#include "g7_preflight.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include "crypto_portable.h"
#include "disk_policy.h"

/* See g7_preflight.h. Every file step is relative to one opened directory, symbolic links are never followed, nothing is ever deleted except what the SAME call
   created a moment before (and only to undo a failed begin). The ECID value is never formatted into any text. */
#define NOTE(...) do { if (note && cap > 0) snprintf(note, (size_t)cap, __VA_ARGS__); } while (0)

static const unsigned char AUX_HEADER[HV_G7_AUX_HEADER_BYTES] = {0xa5, 0xd5, 0xb3, 0xfe, 0x97, 0xad, 0x1d, 0x27, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};

static int open_dir(const char *dir) { return (dir && *dir) ? open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1; }
static int dir_trusted(int dfd) {
    struct stat st;
    return fstat(dfd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & (S_IWGRP | S_IWOTH));
}
/* 1 exists (any kind, link not followed), 0 does not, -1 error */
static int present(int dfd, const char *name, struct stat *st) {
    struct stat local;
    if (!st) st = &local;
    if (fstatat(dfd, name, st, AT_SYMLINK_NOFOLLOW) == 0) return 1;
    return errno == ENOENT ? 0 : -1;
}

/* ---------------------------------------------------------------- ECID */
HvG7Ecid hv_g7_ecid_check(const char *ecid_dir, uint64_t expected) {
    int dfd = open_dir(ecid_dir);
    if (dfd < 0) return HV_G7_ECID_ERROR;
    int fd = openat(dfd, HV_G7_ECID_FILE, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    int e = errno;
    close(dfd);
    if (fd < 0) return e == ENOENT ? HV_G7_ECID_MISSING : HV_G7_ECID_ERROR;
    struct stat st;
    char buf[65];
    ssize_t n = -1;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= 64) n = read(fd, buf, 64);
    close(fd);
    if (n <= 0) return HV_G7_ECID_CORRUPT;
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) buf[--n] = 0;
    if (strncmp(buf, "ecid=0x", 7) || n <= 7 || n > 23) return HV_G7_ECID_CORRUPT;
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(buf + 7, &end, 16);
    if (errno || !end || *end || buf[7] == '-' || buf[7] == '+' || buf[7] == ' ') return HV_G7_ECID_CORRUPT;
    if (v < (1ULL << 40) || v >= (1ULL << 62)) return HV_G7_ECID_CORRUPT;
    return v == expected ? HV_G7_ECID_MATCH : HV_G7_ECID_MISMATCH;
}

/* ---------------------------------------------------------------- ROM */
int hv_g7_rom_check(const char *path, char *note, int cap) {
    if (!path || !*path) { NOTE("no ROM path"); return -1; }
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) { NOTE(errno == ELOOP ? "the ROM is a symbolic link; refused" : "the ROM cannot be opened: %s", strerror(errno)); return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); NOTE("the ROM is not a regular file"); return -1; }
    if ((uint64_t)st.st_size != HV_G7_ROM_BYTES) { close(fd); NOTE("the ROM has %llu bytes, expected %llu", (unsigned long long)st.st_size, (unsigned long long)HV_G7_ROM_BYTES); return -1; }
    uint8_t *buf = malloc(HV_G7_ROM_BYTES);
    size_t got = 0;
    while (buf && got < HV_G7_ROM_BYTES) {
        ssize_t n = read(fd, buf + got, HV_G7_ROM_BYTES - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (!buf || got != HV_G7_ROM_BYTES) { free(buf); NOTE("the ROM could not be read completely"); return -1; }
    uint8_t d[32];
    hv_sha256(buf, got, d);
    free(buf);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    if (strcmp(hex, HV_G7_ROM_SHA256_HEX)) { NOTE("the ROM's SHA-256 is not the expected one"); return -1; }
    NOTE("the ROM has the expected size and SHA-256");
    return 0;
}

/* ---------------------------------------------------------------- pair state */
static int read_state_word(int dfd, char *out, size_t cap) {
    int fd = openat(dfd, HV_G7_STATE_NAME, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    ssize_t n = -1;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < 64) n = read(fd, out, cap - 1);
    close(fd);
    if (n <= 0) return -1;
    out[n] = 0;
    return 0;
}

static HvG7PairState state_at(int dfd, char *note, int cap) {
    const char *names[4] = {HV_INSTALL_DISK_NAME, HV_G7_AUX_NAME, HV_G7_AUX_INITIAL_NAME, HV_G7_STATE_NAME};
    int have[4];
    int any = 0;
    for (int i = 0; i < 4; i++) {
        have[i] = present(dfd, names[i], NULL);
        if (have[i] < 0) { NOTE("cannot look at %s: %s", names[i], strerror(errno)); return HV_G7_PAIR_ERROR; }
        any |= have[i];
    }
    if (!any) { NOTE("no pair, no state file: a clean start"); return HV_G7_PAIR_NONE; }
    if (!have[3]) { NOTE("files of a pair exist but there is no state file: the pair is not trustworthy"); return HV_G7_PAIR_INCONSISTENT; }
    char w[64];
    if (read_state_word(dfd, w, sizeof w) != 0) { NOTE("the state file cannot be read as a plain small file"); return HV_G7_PAIR_INCONSISTENT; }
    if (!have[0] || !have[1] || !have[2]) { NOTE("the state file exists but a member of the pair is missing"); return HV_G7_PAIR_INCONSISTENT; }
    if (!strcmp(w, "state=in_progress\n")) { NOTE("a restore may have been writing (state in_progress): an interrupted or failed pair; it is never reused automatically"); return HV_G7_PAIR_IN_PROGRESS; }
    if (!strcmp(w, "state=done\n")) { NOTE("a pair marked done exists"); return HV_G7_PAIR_DONE; }
    NOTE("the state file has an unknown content");
    return HV_G7_PAIR_INCONSISTENT;
}

HvG7PairState hv_g7_pair_state(const char *dir, char *note, int cap) {
    int dfd = open_dir(dir);
    if (dfd < 0) { NOTE("cannot open the directory: %s", strerror(errno)); return HV_G7_PAIR_ERROR; }
    HvG7PairState s = state_at(dfd, note, cap);
    close(dfd);
    return s;
}

/* ---------------------------------------------------------------- preflight */
static void add(HvG7Report *r, const char *item, HvG7Level lv, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
#include <stdarg.h>
static void add(HvG7Report *r, const char *item, HvG7Level lv, const char *fmt, ...) {
    if (r->n >= HV_G7_MAX_CHECKS) return;
    HvG7Check *c = &r->check[r->n++];
    c->item = item;
    c->level = lv;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->text, sizeof c->text, fmt, ap);
    va_end(ap);
    if (lv == HV_G7_BLOCK) r->blocked++;
}

int hv_g7_preflight(const HvG7Input *in, HvG7Report *rep) {
    HvG7Report local;
    if (!rep) rep = &local;
    memset(rep, 0, sizeof *rep);
    if (!in || !in->dir) { add(rep, "input", HV_G7_BLOCK, "no directory given"); return -1; }
    char note[300];
    int dfd = open_dir(in->dir);
    if (dfd < 0) { add(rep, "directory", HV_G7_BLOCK, "cannot open the pair directory: %s", strerror(errno)); return -1; }
    if (!dir_trusted(dfd)) add(rep, "directory", HV_G7_BLOCK, "the pair directory is not owned by this user or is writable by group/others");
    else add(rep, "directory", HV_G7_PASS, "the pair directory is private");

    HvG7PairState ps = state_at(dfd, note, sizeof note);
    if (ps == HV_G7_PAIR_NONE) add(rep, "state", HV_G7_PASS, "%s", note);
    else add(rep, "state", HV_G7_BLOCK, "%s; a first G7 run needs a clean start (quarantine the old pair first)", note);

    HvDiskInfo di;
    HvDiskState ds = hv_install_disk_check(in->dir, &di, note, sizeof note);
    if (ds == HV_DISK_MISSING) add(rep, "disk", HV_G7_PASS, "%s does not exist yet (a later, separately approved step creates it)", HV_INSTALL_DISK_NAME);
    else add(rep, "disk", HV_G7_BLOCK, "conflict: %s", note);

    struct stat st;
    int p = present(dfd, HV_G7_AUX_NAME, &st);
    if (p == 0) add(rep, "aux", HV_G7_PASS, "%s does not exist yet", HV_G7_AUX_NAME);
    else if (p < 0) add(rep, "aux", HV_G7_BLOCK, "cannot look at %s: %s", HV_G7_AUX_NAME, strerror(errno));
    else if (S_ISLNK(st.st_mode)) add(rep, "aux", HV_G7_BLOCK, "%s is a symbolic link; refused", HV_G7_AUX_NAME);
    else if (!S_ISREG(st.st_mode)) add(rep, "aux", HV_G7_BLOCK, "%s is not a regular file", HV_G7_AUX_NAME);
    else if ((uint64_t)st.st_size != HV_G7_AUX_BYTES) add(rep, "aux", HV_G7_BLOCK, "%s exists with %llu bytes (expected %llu): a conflict, not touched", HV_G7_AUX_NAME, (unsigned long long)st.st_size, (unsigned long long)HV_G7_AUX_BYTES);
    else add(rep, "aux", HV_G7_BLOCK, "%s already exists: a conflict for a first run, not touched", HV_G7_AUX_NAME);

    p = present(dfd, HV_G7_AUX_INITIAL_NAME, NULL);
    if (p == 0) add(rep, "aux_initial", HV_G7_PASS, "no earlier AUX copy");
    else add(rep, "aux_initial", HV_G7_BLOCK, "%s already exists (or cannot be examined): not touched", HV_G7_AUX_INITIAL_NAME);

    if (!in->ecid) add(rep, "ecid", HV_G7_BLOCK, "the request carries no ECID");
    else {
        HvG7Ecid e = hv_g7_ecid_check(in->ecid_dir, in->ecid);
        if (e == HV_G7_ECID_MATCH) add(rep, "ecid", HV_G7_PASS, "the ECID of the request matches %s (value not shown)", HV_G7_ECID_FILE);
        else if (e == HV_G7_ECID_MISMATCH) add(rep, "ecid", HV_G7_BLOCK, "the ECID of the request does NOT match %s (values not shown)", HV_G7_ECID_FILE);
        else if (e == HV_G7_ECID_MISSING) add(rep, "ecid", HV_G7_BLOCK, "%s is missing; none is made here", HV_G7_ECID_FILE);
        else if (e == HV_G7_ECID_CORRUPT) add(rep, "ecid", HV_G7_BLOCK, "%s is not a valid ecid=0x<hex> file in range", HV_G7_ECID_FILE);
        else add(rep, "ecid", HV_G7_BLOCK, "%s cannot be read as a plain file", HV_G7_ECID_FILE);
    }
    close(dfd);

    if (hv_g7_rom_check(in->rom_path, note, sizeof note) == 0) add(rep, "rom", HV_G7_PASS, "%s", note);
    else add(rep, "rom", HV_G7_BLOCK, "%s", note);

    int ifd = open_dir(in->ipsw_dir);
    if (ifd < 0) add(rep, "ipsw", HV_G7_BLOCK, "cannot open the IPSW directory");
    else {
        int pm = present(ifd, HV_G7_IPSW_NAME, &st);
        if (pm <= 0) add(rep, "ipsw", HV_G7_BLOCK, "%s is missing or cannot be examined", HV_G7_IPSW_NAME);
        else if (!S_ISREG(st.st_mode)) add(rep, "ipsw", HV_G7_BLOCK, "%s is not a regular file (a link is refused)", HV_G7_IPSW_NAME);
        else if ((uint64_t)st.st_size != HV_G7_IPSW_BYTES) add(rep, "ipsw", HV_G7_BLOCK, "%s has %llu bytes, expected %llu", HV_G7_IPSW_NAME, (unsigned long long)st.st_size, (unsigned long long)HV_G7_IPSW_BYTES);
        else {
            char part[120], meta[120];
            snprintf(part, sizeof part, "%s.part", HV_G7_IPSW_NAME);
            snprintf(meta, sizeof meta, "%s.meta", HV_G7_IPSW_NAME);
            if (present(ifd, part, NULL) != 0 || present(ifd, meta, NULL) != 0) add(rep, "ipsw", HV_G7_BLOCK, "the size is right but a .part/.meta of an unfinished download sits next to it");
            else add(rep, "ipsw", HV_G7_PASS, "the IPSW has the expected size (the SHA-256 is checked by the downloader before it installs the final name, not here)");
        }
        close(ifd);
    }

    struct statvfs sv;
    if (statvfs(in->dir, &sv) != 0) add(rep, "space", HV_G7_BLOCK, "cannot read the free space");
    else {
        uint64_t freeb = (uint64_t)sv.f_bavail * sv.f_frsize;
        uint64_t need = HV_G7_MIN_FREE_BYTES + in->extra_free_bytes;
        if (need < in->extra_free_bytes) need = ~0ULL;
        if (freeb < need) add(rep, "space", HV_G7_BLOCK, "%llu bytes free, at least %llu are required (a sparse disk grows as the guest writes)", (unsigned long long)freeb, (unsigned long long)need);
        else add(rep, "space", HV_G7_PASS, "%llu bytes free (a floor, not a guarantee)", (unsigned long long)freeb);
    }
    return rep->blocked ? -1 : 0;
}

void hv_g7_report_text(const HvG7Report *rep, char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    size_t o = 0;
    for (int i = 0; rep && i < rep->n && o + 1 < cap; i++) {
        int w = snprintf(out + o, cap - o, "%s %-11s %s\n", rep->check[i].level == HV_G7_PASS ? "PASS " : "BLOCK", rep->check[i].item, rep->check[i].text);
        if (w < 0) break;
        o += (size_t)w >= cap - o ? cap - o - 1 : (size_t)w;
    }
}

/* ---------------------------------------------------------------- the only writable configuration */
int hv_g7_config_ok(const HvVmConfig *c, const char *dir, uint64_t expected_ecid, char *why, size_t cap) {
#define BAD(msg) do { if (why && cap) snprintf(why, cap, "%s", msg); return -1; } while (0)
    if (!c || !dir || !*dir) BAD("no configuration or no directory");
    if (!c->installer) BAD("G7 needs the installer boot");
    if (c->readonly_storage) BAD("a G7 configuration has writable storage (read-only is the G0-G6 contract)");
    if (!expected_ecid || c->ecid != expected_ecid) BAD("the machine identity does not match the one the request was checked with");
    if (c->seed) BAD("a seed is set (the installer uses 0)");
    if (!c->rom || !*c->rom) BAD("no ROM path");
    char want[4096];
    if (!c->disk || snprintf(want, sizeof want, "%s/%s", dir, HV_INSTALL_DISK_NAME) >= (int)sizeof want || strcmp(c->disk, want)) BAD("the disk is not exactly <dir>/macos_disk.img");
    if (!c->aux || snprintf(want, sizeof want, "%s/%s", dir, HV_G7_AUX_NAME) >= (int)sizeof want || strcmp(c->aux, want)) BAD("the AUX is not exactly <dir>/macos_aux.img");
    if (c->aux_offset != 0 && c->aux_offset != 16384) BAD("aux_offset is not 16384");
    if (c->ram_mb < 2048 || c->ram_mb > 4096) BAD("RAM size out of range (2048..4096 MB)");
    if (c->ncpus < 1 || c->ncpus > 4) BAD("CPU count out of range (1..4)");
    if (c->usb_on_normal_boot || c->usb_probe_on_normal_boot) BAD("a host USB diagnostic is on");
    if (why && cap) why[0] = 0;
    return 0;
#undef BAD
}

/* ---------------------------------------------------------------- the pair */
static int write_all(int fd, const void *b, size_t n) {
    const unsigned char *p = b;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int make_aux_at(int dfd, const char *name) {
    int fd = openat(dfd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int ok = write_all(fd, AUX_HEADER, sizeof AUX_HEADER) == 0 && ftruncate(fd, (off_t)HV_G7_AUX_BYTES) == 0 && fsync(fd) == 0;
    close(fd);
    return ok ? 0 : -1;
}

/* a byte copy of the AUX; all-zero 4 KiB blocks stay holes */
static int copy_aux(int dfd, const char *from, const char *to) {
    int in = openat(dfd, from, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (in < 0) return -1;
    int out = openat(dfd, to, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (out < 0) { close(in); return -1; }
    struct stat st;
    int ok = fstat(in, &st) == 0 && S_ISREG(st.st_mode) && (uint64_t)st.st_size == HV_G7_AUX_BYTES && ftruncate(out, st.st_size) == 0;
    static unsigned char buf[4096], zero[4096];
    for (off_t off = 0; ok && off < st.st_size; off += (off_t)sizeof buf) {
        ssize_t n = pread(in, buf, sizeof buf, off);
        if (n <= 0) { ok = 0; break; }
        if (memcmp(buf, zero, (size_t)n) && pwrite(out, buf, (size_t)n, off) != n) ok = 0;
    }
    if (ok && fsync(out) != 0) ok = 0;
    close(in);
    close(out);
    return ok ? 0 : -1;
}

int hv_g7_pair_begin(const char *dir, HvG7DiskCreate create_disk, char *note, int cap) {
    if (!create_disk) { NOTE("not started: no disk creator was given"); return -1; }
    int dfd = open_dir(dir);
    if (dfd < 0) { NOTE("cannot open the directory: %s", strerror(errno)); return -1; }
    if (!dir_trusted(dfd)) { close(dfd); NOTE("refused: the directory is not owned by this user or is writable by others"); return -1; }
    char n2[200];
    HvG7PairState s = state_at(dfd, n2, sizeof n2);
    if (s != HV_G7_PAIR_NONE) { close(dfd); NOTE("not started: %s", n2); return -1; }
    int made_state = 0, made_disk = 0, made_aux = 0, made_init = 0;
    const char *fail = NULL;
    int sfd = openat(dfd, HV_G7_STATE_NAME, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (sfd < 0) fail = "the state file";
    else {
        made_state = 1;
        if (write_all(sfd, "state=in_progress\n", 18) != 0 || fsync(sfd) != 0) fail = "writing the state file";
        close(sfd);
    }
    if (!fail) {
        if (create_disk(dir, n2, sizeof n2) != 0) fail = "the disk";
        else made_disk = 1;
    }
    if (!fail) { if (make_aux_at(dfd, HV_G7_AUX_NAME) != 0) { fail = "the AUX"; made_aux = present(dfd, HV_G7_AUX_NAME, NULL) == 1; } else made_aux = 1; }
    if (!fail) { if (copy_aux(dfd, HV_G7_AUX_NAME, HV_G7_AUX_INITIAL_NAME) != 0) { fail = "the AUX copy"; made_init = present(dfd, HV_G7_AUX_INITIAL_NAME, NULL) == 1; } else made_init = 1; }
    if (fail) {
        /* undo only what this call made; the files were created with O_EXCL a moment ago by us */
        if (made_init) unlinkat(dfd, HV_G7_AUX_INITIAL_NAME, 0);
        if (made_aux) unlinkat(dfd, HV_G7_AUX_NAME, 0);
        if (made_disk) unlinkat(dfd, HV_INSTALL_DISK_NAME, 0);
        if (made_state) unlinkat(dfd, HV_G7_STATE_NAME, 0);
        close(dfd);
        NOTE("not started: could not make %s; what this call made was removed again", fail);
        return -1;
    }
    close(dfd);
    NOTE("a pair was made: disk, AUX, a copy of the AUX, state=in_progress");
    return 0;
}

int hv_g7_pair_finish(const char *dir, char *note, int cap) {
    int dfd = open_dir(dir);
    if (dfd < 0) { NOTE("cannot open the directory: %s", strerror(errno)); return -1; }
    char n2[200];
    if (state_at(dfd, n2, sizeof n2) != HV_G7_PAIR_IN_PROGRESS) { close(dfd); NOTE("not finished: %s", n2); return -1; }
    const char *tmp = HV_G7_STATE_NAME ".new";
    int fd = openat(dfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { close(dfd); NOTE("not finished: cannot make the new state file: %s", strerror(errno)); return -1; }
    int ok = write_all(fd, "state=done\n", 11) == 0 && fsync(fd) == 0;
    close(fd);
    if (ok && renameat(dfd, tmp, dfd, HV_G7_STATE_NAME) != 0) ok = 0;
    if (!ok) unlinkat(dfd, tmp, 0);
    close(dfd);
    if (!ok) { NOTE("not finished: the state file was not replaced (still in_progress)"); return -1; }
    NOTE("state=done");
    return 0;
}

static int tag_ok(const char *t) {
    size_t n = t ? strlen(t) : 0;
    if (n < 1 || n > 32) return 0;
    for (size_t i = 0; i < n; i++) if (!((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= '0' && t[i] <= '9') || t[i] == '_' || t[i] == '-')) return 0;
    return 1;
}

int hv_g7_pair_quarantine(const char *dir, const char *tag, char *note, int cap) {
    if (!tag_ok(tag)) { NOTE("not moved: the tag must be 1..32 of [a-z0-9_-]"); return -1; }
    int dfd = open_dir(dir);
    if (dfd < 0) { NOTE("cannot open the directory: %s", strerror(errno)); return -1; }
    if (!dir_trusted(dfd)) { close(dfd); NOTE("refused: the directory is not owned by this user or is writable by others"); return -1; }
    char n2[200];
    HvG7PairState s = state_at(dfd, n2, sizeof n2);
    if (s == HV_G7_PAIR_NONE || s == HV_G7_PAIR_ERROR) { close(dfd); NOTE("nothing moved: %s", n2); return -1; }
    char qn[96];
    snprintf(qn, sizeof qn, "%s%s", HV_G7_QUARANTINE_PREFIX, tag);
    if (mkdirat(dfd, qn, 0700) != 0) { int e = errno; close(dfd); NOTE("not moved: cannot make %s: %s", qn, strerror(e)); return -1; }
    int qfd = openat(dfd, qn, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (qfd < 0) { close(dfd); NOTE("not moved: cannot open %s", qn); return -1; }
    const char *names[4] = {HV_INSTALL_DISK_NAME, HV_G7_AUX_NAME, HV_G7_AUX_INITIAL_NAME, HV_G7_STATE_NAME};
    int moved = 0;
    for (int i = 0; i < 4; i++) {
        int p = present(dfd, names[i], NULL);
        if (p == 0) continue;
        if (p < 0 || renameat(dfd, names[i], qfd, names[i]) != 0) {
            int e = errno;
            close(qfd); close(dfd);
            NOTE("moved %d item(s), then %s could not be moved: %s; nothing was deleted", moved, names[i], strerror(e));
            return -1;
        }
        moved++;
    }
    close(qfd);
    close(dfd);
    NOTE("moved %d item(s) into %s; nothing was deleted or overwritten", moved, qn);
    return moved;
}
