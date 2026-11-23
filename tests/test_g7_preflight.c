/* host/g7_preflight.c in temporary directories, with a MOCK writer in place of the restore: no machine, no restore engine, no network, no Apple. Linux.
   Part 1 the configuration contract (G0-G6 and the [A] runner keep refusing writable storage; only hv_g7_config_ok accepts it)
   Part 2 the read-only preflight (missing / wrong size / link / conflict / ECID / IPSW / space), and that it changes nothing and prints no ECID
   Part 3 the pair (begin / finish / quarantine / rollback) and the mock writes (ok, out of space, partial write, interruption, restart) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../host/disk.h"
#include "../host/disk_policy.h"
#include "../host/g7_preflight.h"
#include "../host/runner_vm_core.h"

int hv_knob[HV_K_COUNT];   /* host/disk.c reads the flush knob; the engine that owns it is not linked here */

static int g_fail;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

#define ECID_REQ 0x0013579bdf024681ULL            /* inside [2^40, 2^62) */
#define ECID_REQ_HEX "13579bdf024681"
#define ECID_OTHER_HEX "2468ace13579bd"

static char T[200];
static char G_ROM[260];
/* the stand-in ROM: HV_G7_ROM_BYTES of a fixed pattern; tools/g7-preflight-test.sh builds the same bytes and passes their SHA-256 as HV_G7_ROM_SHA256_HEX */
static void make_rom(const char *path, int flip) {
    FILE *f = fopen(path, "wb");
    for (unsigned long i = 0; f && i < HV_G7_ROM_BYTES; i++) { unsigned char b = (unsigned char)(i * 31 + 7); if (flip && i == 1000) b ^= 1; fputc(b, f); }
    if (f) fclose(f);
}
static char *newdir(const char *tag) {
    static char d[64][240];
    static int k;
    char *p = d[k++ % 64];
    snprintf(p, 240, "%s/%s", T, tag);
    mkdir(p, 0700);
    chmod(p, 0700);
    return p;
}
static void put(const char *dir, const char *name, const void *b, size_t n) {
    char p[400]; snprintf(p, sizeof p, "%s/%s", dir, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600); if (fd >= 0) { if (n) (void)!write(fd, b, n); close(fd); }
}
static void sparse(const char *dir, const char *name, unsigned long long size) {
    char p[400]; snprintf(p, sizeof p, "%s/%s", dir, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600); if (fd >= 0) { (void)!ftruncate(fd, (off_t)size); close(fd); }
}
static void put_ecid(const char *dir, const char *hex) { char b[80]; snprintf(b, sizeof b, "ecid=0x%s\n", hex); put(dir, HV_G7_ECID_FILE, b, strlen(b)); }
static int exists(const char *dir, const char *name) { char p[400]; struct stat st; snprintf(p, sizeof p, "%s/%s", dir, name); return lstat(p, &st) == 0; }

/* a snapshot of one directory: names, sizes, mtimes, link targets */
static void snap(const char *dir, char *out, size_t cap) {
    DIR *d = opendir(dir); struct dirent *e; char names[64][80]; int n = 0;
    out[0] = 0;
    while (d && (e = readdir(d)) && n < 64) { memset(names[n], 0, 80); strncpy(names[n++], e->d_name, 79); }
    if (d) closedir(d);
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (strcmp(names[i], names[j]) > 0) { char t[80]; memcpy(t, names[i], 80); memcpy(names[i], names[j], 80); memcpy(names[j], t, 80); }
    for (int i = 0; i < n; i++) {
        char p[400], l[300] = ""; struct stat st; snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        if (lstat(p, &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) { ssize_t r = readlink(p, l, sizeof l - 1); if (r > 0) l[r] = 0; }
        size_t o = strlen(out);
        snprintf(out + o, cap - o, "%s %llu %lld.%ld %s|", names[i], (unsigned long long)st.st_size, (long long)st.st_mtim.tv_sec, st.st_mtim.tv_nsec, l);
    }
}

static int has(const HvG7Report *r, const char *item, HvG7Level lv) {
    for (int i = 0; i < r->n; i++) if (!strcmp(r->check[i].item, item)) return r->check[i].level == lv;
    return 0;
}
static int leaks_ecid(const HvG7Report *r) {
    char t[4096]; hv_g7_report_text(r, t, sizeof t);
    return strcasestr(t, ECID_REQ_HEX) || strcasestr(t, ECID_OTHER_HEX) || strstr(t, "0x1");
}

static HvG7Input input(const char *dir, const char *ipsw_dir, const char *ecid_dir) {
    HvG7Input in = {.dir = dir, .ipsw_dir = ipsw_dir, .ecid_dir = ecid_dir, .rom_path = G_ROM, .ecid = ECID_REQ};
    return in;
}
/* a directory set where everything is as a first G7 run needs it */
static void good_inputs(const char *dir, const char *ipsw_dir, const char *ecid_dir) {
    sparse(ipsw_dir, HV_G7_IPSW_NAME, HV_G7_IPSW_BYTES);
    put_ecid(ecid_dir, ECID_REQ_HEX);
    (void)dir;
}

/* ------------------------------------------------------------------ mock restore */
typedef struct { int req, ok, failed; unsigned long long bytes_ok; } Tally;
static int pat_at(unsigned char *b, size_t n, unsigned char seed) { for (size_t i = 0; i < n; i++) b[i] = (unsigned char)(seed + i * 7u); return 0; }
static void mock_write(HvDisk *d, uint64_t off, size_t len, unsigned char seed, Tally *t) {
    unsigned char *b = malloc(len); pat_at(b, len, seed);
    t->req++;
    if (hv_disk_write(d, off, b, len) == 0) { t->ok++; t->bytes_ok += len; } else t->failed++;
    free(b);
}
/* how many bytes at [off, off+len) really hold the pattern (read back from the file, independent of any counter) */
static size_t readback_ok(HvDisk *d, uint64_t off, size_t len, unsigned char seed) {
    unsigned char *want = malloc(len), *got = malloc(len); pat_at(want, len, seed);
    size_t good = 0;
    if (hv_disk_read(d, off, got, len) == 0) while (good < len && got[good] == want[good]) good++;
    free(want); free(got);
    return good;
}
static int same_file(const char *dir, const char *a, const char *b) {
    char pa[400], pb[400]; snprintf(pa, sizeof pa, "%s/%s", dir, a); snprintf(pb, sizeof pb, "%s/%s", dir, b);
    FILE *fa = fopen(pa, "rb"), *fb = fopen(pb, "rb"); int same = fa && fb;
    while (same) { unsigned char x[4096], y[4096]; size_t n = fread(x, 1, sizeof x, fa), m = fread(y, 1, sizeof y, fb); if (n != m || memcmp(x, y, n)) same = 0; if (n == 0) break; }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

int main(void) {
    snprintf(T, sizeof T, "/tmp/hvg7-XXXXXX");
    if (!mkdtemp(T)) return 2;
    signal(SIGXFSZ, SIG_IGN);
    snprintf(G_ROM, sizeof G_ROM, "%s/rom.bin", T);
    make_rom(G_ROM, 0);
    char note[400];
    HvG7Report rep;

    /* ============================================================ Part 1: configuration contract */
    printf("--- 1. configuration contract\n");
    {
        char why[200]; HvVmConfig c;
        hv_runner_make_config(&c, "/r/rom.bin", "/d/blank_disk.img", "/d/blank_aux.img", 1024, 2, 64);
        CHECK(hv_runner_config_ok(&c, why, sizeof why) == 0, "the [A]/G0-G6 runner config is accepted by its own validator");
        CHECK(hv_g7_config_ok(&c, "/d", ECID_REQ, why, sizeof why) == -1, "...and refused by the G7 validator (%s)", why);
        c.readonly_storage = 0;
        CHECK(hv_runner_config_ok(&c, why, sizeof why) == -1, "the runner validator refuses readonly_storage=0 (%s)", why);

        HvVmConfig g = {0};
        g.rom = "/r/rom.bin"; g.disk = "/d/macos_disk.img"; g.aux = "/d/macos_aux.img"; g.aux_offset = 16384; g.ecid = ECID_REQ;
        g.ram_mb = 2048; g.ncpus = 2; g.installer = 1; g.no_sound = 1; g.readonly_storage = 0;
        CHECK(hv_g7_config_ok(&g, "/d", ECID_REQ, why, sizeof why) == 0, "a complete G7 configuration is accepted by the G7 validator");
        CHECK(hv_runner_config_ok(&g, why, sizeof why) == -1, "the same configuration is refused by the runner validator (%s)", why);
        HvVmConfig x;
#define VARIANT(label, stmt) do { x = g; stmt; CHECK(hv_g7_config_ok(&x, "/d", ECID_REQ, why, sizeof why) == -1, "G7 validator refuses: %s (%s)", label, why); } while (0)
        VARIANT("read-only storage", x.readonly_storage = 1);
        VARIANT("no installer", x.installer = 0);
        VARIANT("other ECID", x.ecid = ECID_REQ + 1);
        VARIANT("no ECID", x.ecid = 0);
        VARIANT("a seed", x.seed = 5);
        VARIANT("a blank_ disk", x.disk = "/d/blank_disk.img");
        VARIANT("a disk in another directory", x.disk = "/e/macos_disk.img");
        VARIANT("a disk path with a trailing part", x.disk = "/d/macos_disk.img.x");
        VARIANT("a different AUX", x.aux = "/d/blank_aux.img");
        VARIANT("a different aux offset", x.aux_offset = 0x1000);
        VARIANT("too little RAM", x.ram_mb = 512);
        VARIANT("too many CPUs", x.ncpus = 9);
        VARIANT("a USB diagnostic", x.usb_on_normal_boot = 1);
        VARIANT("no ROM", x.rom = NULL);
        CHECK(hv_g7_config_ok(&g, "/d", 0, why, sizeof why) == -1, "an expected ECID of 0 is refused");
        CHECK(hv_g7_config_ok(NULL, "/d", ECID_REQ, why, sizeof why) == -1, "no configuration is refused");
    }

    /* ============================================================ Part 2: preflight */
    printf("--- 2. read-only preflight\n");
    HvG7Input in;
    {
        char *d = newdir("clean"), *ip = newdir("clean_ipsw"), *ec = newdir("clean_ecid");
        good_inputs(d, ip, ec);
        in = input(d, ip, ec);
        char before[8000], after[8000], b2[8000], a2[8000];
        snap(d, before, sizeof before); snap(ip, b2, sizeof b2);
        int r = hv_g7_preflight(&in, &rep);
        snap(d, after, sizeof after); snap(ip, a2, sizeof a2);
        CHECK(r == 0 && rep.blocked == 0, "a clean start passes (%d items)", rep.n);
        CHECK(!strcmp(before, after) && !strcmp(b2, a2), "the preflight changed nothing (names, sizes, mtimes)");
        CHECK(!leaks_ecid(&rep), "no ECID value in the report");
        char t[4096]; hv_g7_report_text(&rep, t, sizeof t); printf("%s", t);
    }
    {   /* disk */
        char *d = newdir("p_disk"), *ip = newdir("p_disk_ipsw"), *ec = newdir("p_disk_ecid"); good_inputs(d, ip, ec); in = input(d, ip, ec);
        sparse(d, HV_INSTALL_DISK_NAME, HV_INSTALL_DISK_BYTES);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "disk", HV_G7_BLOCK), "an existing macos_disk.img is a conflict");
        CHECK(has(&rep, "state", HV_G7_BLOCK), "...and, with no state file, an inconsistent pair");
        sparse(d, HV_INSTALL_DISK_NAME, 1ULL << 30);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "disk", HV_G7_BLOCK), "a wrong-size disk blocks");
        unlink("/dev/null/x");
        char p[400]; snprintf(p, sizeof p, "%s/%s", d, HV_INSTALL_DISK_NAME); unlink(p);
        char v[400]; snprintf(v, sizeof v, "%s/victim", d); put(d, "victim", "x", 1);
        symlink(v, p);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "disk", HV_G7_BLOCK), "a symbolic link as the disk blocks");
    }
    {   /* aux */
        char *d = newdir("p_aux"), *ip = newdir("p_aux_ipsw"), *ec = newdir("p_aux_ecid"); good_inputs(d, ip, ec); in = input(d, ip, ec);
        sparse(d, HV_G7_AUX_NAME, HV_G7_AUX_BYTES);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "aux", HV_G7_BLOCK), "an existing AUX of the right size is a conflict");
        sparse(d, HV_G7_AUX_NAME, 12345);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "aux", HV_G7_BLOCK), "an AUX of a wrong size blocks");
        char p[400]; snprintf(p, sizeof p, "%s/%s", d, HV_G7_AUX_NAME); unlink(p);
        put(d, "victim", "x", 1); char v[400]; snprintf(v, sizeof v, "%s/victim", d); symlink(v, p);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "aux", HV_G7_BLOCK), "a symbolic link as the AUX blocks");
        unlink(p); mkdir(p, 0700);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "aux", HV_G7_BLOCK), "a directory as the AUX blocks");
        rmdir(p);
        put(d, HV_G7_AUX_INITIAL_NAME, "x", 1);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "aux_initial", HV_G7_BLOCK), "a leftover AUX copy blocks");
    }
    {   /* state file / interrupted */
        char *d = newdir("p_state"), *ip = newdir("p_state_ipsw"), *ec = newdir("p_state_ecid"); good_inputs(d, ip, ec); in = input(d, ip, ec);
        put(d, HV_G7_STATE_NAME, "state=in_progress\n", 18);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "state", HV_G7_BLOCK), "a lone state file blocks");
    }
    {   /* ECID */
        char *d = newdir("p_ecid"), *ip = newdir("p_ecid_ipsw"), *ec = newdir("p_ecid_ecid"); sparse(ip, HV_G7_IPSW_NAME, HV_G7_IPSW_BYTES); in = input(d, ip, ec);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "ecid", HV_G7_BLOCK) && hv_g7_ecid_check(ec, ECID_REQ) == HV_G7_ECID_MISSING, "no vm_ecid.txt: blocked, and none is created");
        CHECK(!exists(ec, HV_G7_ECID_FILE), "...the file was not created");
        put_ecid(ec, ECID_OTHER_HEX);
        CHECK(hv_g7_ecid_check(ec, ECID_REQ) == HV_G7_ECID_MISMATCH, "a different ECID: mismatch");
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "ecid", HV_G7_BLOCK) && !leaks_ecid(&rep), "...blocked, and neither value appears in the report");
        put_ecid(ec, ECID_REQ_HEX);
        CHECK(hv_g7_ecid_check(ec, ECID_REQ) == HV_G7_ECID_MATCH && hv_g7_preflight(&in, &rep) == 0 && !leaks_ecid(&rep), "the same ECID: match, preflight passes, value not shown");
        const char *bad[] = {"ecid=0x1\n", "ecid=0x\n", "ecid=1234\n", "ecid=0xzz\n", "ecid=0x4000000000000000\n", "ecid=0x-13579bdf024681\n", "ecid=0x13579bdf024681 trailing\n", "\n"};
        for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) { put(ec, HV_G7_ECID_FILE, bad[i], strlen(bad[i])); CHECK(hv_g7_ecid_check(ec, ECID_REQ) == HV_G7_ECID_CORRUPT, "corrupt/out-of-range text #%u is CORRUPT", i); }
        char big[200]; memset(big, 'a', sizeof big); put(ec, HV_G7_ECID_FILE, big, sizeof big);
        CHECK(hv_g7_ecid_check(ec, ECID_REQ) == HV_G7_ECID_CORRUPT, "an oversized file is CORRUPT");
        char p[400]; snprintf(p, sizeof p, "%s/%s", ec, HV_G7_ECID_FILE); unlink(p);
        put(d, "real_ecid", "ecid=0x" ECID_REQ_HEX "\n", 7 + 14 + 1); char v[400]; snprintf(v, sizeof v, "%s/real_ecid", d); symlink(v, p);
        CHECK(hv_g7_ecid_check(ec, ECID_REQ) != HV_G7_ECID_MATCH, "a symbolic link as vm_ecid.txt is not followed");
        in.ecid = 0;
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "ecid", HV_G7_BLOCK), "a request without an ECID blocks");
    }
    {   /* IPSW */
        char *d = newdir("p_ipsw"), *ip = newdir("p_ipsw_ipsw"), *ec = newdir("p_ipsw_ecid"); put_ecid(ec, ECID_REQ_HEX); in = input(d, ip, ec);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "ipsw", HV_G7_BLOCK), "no IPSW blocks");
        sparse(ip, HV_G7_IPSW_NAME, HV_G7_IPSW_BYTES - 1);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "ipsw", HV_G7_BLOCK), "an IPSW one byte short blocks");
        sparse(ip, HV_G7_IPSW_NAME, HV_G7_IPSW_BYTES);
        CHECK(hv_g7_preflight(&in, &rep) == 0, "the right size passes");
        put(ip, HV_G7_IPSW_NAME ".part", "x", 1);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "ipsw", HV_G7_BLOCK), "a .part next to it blocks");
        char p[400]; snprintf(p, sizeof p, "%s/%s.part", ip, HV_G7_IPSW_NAME); unlink(p);
        put(ip, HV_G7_IPSW_NAME ".meta", "x", 1);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "ipsw", HV_G7_BLOCK), "a .meta next to it blocks");
        snprintf(p, sizeof p, "%s/%s.meta", ip, HV_G7_IPSW_NAME); unlink(p);
        snprintf(p, sizeof p, "%s/%s", ip, HV_G7_IPSW_NAME); unlink(p);
        sparse(d, "other.ipsw", HV_G7_IPSW_BYTES); char t[400]; snprintf(t, sizeof t, "%s/other.ipsw", d); symlink(t, p);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "ipsw", HV_G7_BLOCK), "a symbolic link as the IPSW blocks");
    }
    {   /* ROM */
        char *d = newdir("p_rom"), *ip = newdir("p_rom_ipsw"), *ec = newdir("p_rom_ecid"); good_inputs(d, ip, ec); in = input(d, ip, ec);
        char note2[200];
        CHECK(hv_g7_preflight(&in, &rep) == 0 && has(&rep, "rom", HV_G7_PASS), "the ROM with the expected size and SHA-256 passes");
        CHECK(hv_g7_rom_check(G_ROM, note2, sizeof note2) == 0, "rom check: %s", note2);
        make_rom(G_ROM, 1);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "rom", HV_G7_BLOCK), "one flipped byte: the SHA-256 differs, blocked");
        truncate(G_ROM, HV_G7_ROM_BYTES - 1);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "rom", HV_G7_BLOCK), "one byte short: blocked");
        truncate(G_ROM, HV_G7_ROM_BYTES + 1);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "rom", HV_G7_BLOCK), "one byte long: blocked");
        unlink(G_ROM);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "rom", HV_G7_BLOCK), "missing: blocked");
        char real[300]; snprintf(real, sizeof real, "%s/rom_real.bin", T); make_rom(real, 0); symlink(real, G_ROM);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "rom", HV_G7_BLOCK), "a symbolic link to a good ROM: refused");
        unlink(G_ROM); mkdir(G_ROM, 0700);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "rom", HV_G7_BLOCK), "a directory: refused");
        rmdir(G_ROM);
        in.rom_path = NULL;
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "rom", HV_G7_BLOCK), "no ROM path: blocked");
        make_rom(G_ROM, 0);
        chmod(G_ROM, 0600);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "rom", HV_G7_BLOCK), "(control) still blocked while the input has no ROM path");
        in.rom_path = G_ROM;
        CHECK(hv_g7_preflight(&in, &rep) == 0, "restored: passes again");
    }
    {   /* directory + space */
        char *d = newdir("p_dir"), *ip = newdir("p_dir_ipsw"), *ec = newdir("p_dir_ecid"); good_inputs(d, ip, ec); in = input(d, ip, ec);
        chmod(d, 0777);
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "directory", HV_G7_BLOCK), "a group/world-writable directory blocks");
        chmod(d, 0700);
        in.extra_free_bytes = ~0ULL;
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "space", HV_G7_BLOCK), "not enough free space blocks");
        in.extra_free_bytes = 0;
        in.dir = "/nonexistent/g7";
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "directory", HV_G7_BLOCK), "a missing directory blocks");
        CHECK(hv_g7_preflight(NULL, &rep) == -1, "no input blocks");
    }

    /* ============================================================ Part 3: pair + mock writes */
    printf("--- 3. the pair and the mock writes\n");
    {
        char *d = newdir("pair"), *ip = newdir("pair_ipsw"), *ec = newdir("pair_ecid"); good_inputs(d, ip, ec); in = input(d, ip, ec);
        CHECK(hv_g7_pair_state(d, note, sizeof note) == HV_G7_PAIR_NONE, "a new directory has no pair");
        CHECK(hv_g7_pair_begin(d, NULL, note, sizeof note) == -1 && hv_g7_pair_state(d, NULL, 0) == HV_G7_PAIR_NONE, "begin without a disk creator is refused and leaves nothing");
        CHECK(hv_g7_pair_begin(d, hv_install_disk_create, note, sizeof note) == 0, "begin: %s", note);
        CHECK(hv_g7_pair_state(d, note, sizeof note) == HV_G7_PAIR_IN_PROGRESS, "state is in_progress");
        struct stat sd, sa, si; char p[400];
        snprintf(p, sizeof p, "%s/%s", d, HV_INSTALL_DISK_NAME); stat(p, &sd);
        snprintf(p, sizeof p, "%s/%s", d, HV_G7_AUX_NAME); stat(p, &sa);
        snprintf(p, sizeof p, "%s/%s", d, HV_G7_AUX_INITIAL_NAME); stat(p, &si);
        CHECK((uint64_t)sd.st_size == HV_INSTALL_DISK_BYTES && (uint64_t)sd.st_blocks * 512 < (64ULL << 20), "disk: 128 GiB, sparse");
        CHECK((uint64_t)sa.st_size == HV_G7_AUX_BYTES && (uint64_t)si.st_size == HV_G7_AUX_BYTES, "AUX and its copy: 33,570,816 bytes");
        CHECK(same_file(d, HV_G7_AUX_NAME, HV_G7_AUX_INITIAL_NAME), "the AUX copy is byte-identical to the AUX");
        CHECK((sa.st_mode & 077) == 0 && (si.st_mode & 077) == 0 && (sd.st_mode & 077) == 0, "all three files are 0600");
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "state", HV_G7_BLOCK), "a second preflight now blocks (a pair exists)");
        CHECK(hv_g7_pair_begin(d, hv_install_disk_create, note, sizeof note) == -1, "a second begin is refused: %s", note);
        CHECK(hv_g7_pair_finish(d, note, sizeof note) == 0 && hv_g7_pair_state(d, NULL, 0) == HV_G7_PAIR_DONE, "finish: in_progress -> done");
        CHECK(hv_g7_pair_finish(d, note, sizeof note) == -1, "finish again is refused (%s)", note);
        CHECK(hv_g7_pair_begin(d, hv_install_disk_create, note, sizeof note) == -1, "begin on a done pair is refused");
        CHECK(!exists(d, HV_G7_STATE_NAME ".new"), "no temporary state file is left");
        CHECK(hv_g7_pair_quarantine(d, "../x", note, sizeof note) == -1 && hv_g7_pair_quarantine(d, "", note, sizeof note) == -1 && hv_g7_pair_quarantine(d, "UP", note, sizeof note) == -1, "bad quarantine tags are refused");
        CHECK(hv_g7_pair_quarantine(d, "t1", note, sizeof note) == 4, "quarantine moves 4 items: %s", note);
        CHECK(hv_g7_pair_state(d, NULL, 0) == HV_G7_PAIR_NONE && !exists(d, HV_INSTALL_DISK_NAME) && !exists(d, HV_G7_AUX_NAME), "the directory is clean again");
        char q[400]; snprintf(q, sizeof q, "%s/%st1", d, HV_G7_QUARANTINE_PREFIX);
        CHECK(exists(q, HV_INSTALL_DISK_NAME) && exists(q, HV_G7_AUX_NAME) && exists(q, HV_G7_AUX_INITIAL_NAME) && exists(q, HV_G7_STATE_NAME), "all four are in the quarantine directory, none deleted");
        CHECK(hv_g7_preflight(&in, &rep) == 0, "preflight passes again after the quarantine");
        CHECK(hv_g7_pair_quarantine(d, "t2", note, sizeof note) == -1, "quarantine with nothing to move: refused (%s)", note);
        CHECK(hv_g7_pair_begin(d, hv_install_disk_create, note, sizeof note) == 0, "a new pair can begin");
        CHECK(hv_g7_pair_quarantine(d, "t1", note, sizeof note) == -1 && hv_g7_pair_state(d, NULL, 0) == HV_G7_PAIR_IN_PROGRESS, "quarantine into an existing tag is refused and the new pair is untouched");
        CHECK(exists(q, HV_INSTALL_DISK_NAME), "the old quarantined pair is intact");
    }
    {   /* rollback: the disk step fails (file size limit), so the state file made first must go again */
        char *d = newdir("rollback");
        struct rlimit old, lim; getrlimit(RLIMIT_FSIZE, &old); lim = old; lim.rlim_cur = 1ULL << 30; setrlimit(RLIMIT_FSIZE, &lim);
        int r = hv_g7_pair_begin(d, hv_install_disk_create, note, sizeof note);
        setrlimit(RLIMIT_FSIZE, &old);
        CHECK(r == -1, "begin fails when the disk cannot be sized (%s)", note);
        DIR *dd = opendir(d); int n = 0; struct dirent *e; while ((e = readdir(dd))) if (e->d_name[0] != '.') n++; closedir(dd);
        CHECK(n == 0, "and the directory is empty again (what the call made was removed)");
        CHECK(hv_g7_pair_state(d, NULL, 0) == HV_G7_PAIR_NONE, "state is NONE after the failed begin");
    }
    {   /* inconsistent states */
        char *d = newdir("incons");
        put(d, HV_G7_STATE_NAME, "state=in_progress\n", 18);
        CHECK(hv_g7_pair_state(d, NULL, 0) == HV_G7_PAIR_INCONSISTENT, "a state file without the pair is INCONSISTENT");
        CHECK(hv_g7_pair_begin(d, hv_install_disk_create, note, sizeof note) == -1, "begin refuses it");
        CHECK(hv_g7_pair_quarantine(d, "inc", note, sizeof note) == 1, "quarantine can still set it aside (%s)", note);
        char *d2 = newdir("incons2");
        sparse(d2, HV_G7_AUX_NAME, HV_G7_AUX_BYTES);
        CHECK(hv_g7_pair_state(d2, NULL, 0) == HV_G7_PAIR_INCONSISTENT, "an AUX without a state file is INCONSISTENT");
        put(d2, HV_G7_STATE_NAME, "garbage", 7); sparse(d2, HV_INSTALL_DISK_NAME, 1); put(d2, HV_G7_AUX_INITIAL_NAME, "x", 1);
        CHECK(hv_g7_pair_state(d2, NULL, 0) == HV_G7_PAIR_INCONSISTENT, "an unknown state content is INCONSISTENT");
        CHECK(hv_g7_pair_finish(d2, note, sizeof note) == -1, "finish refuses it");
    }
    {   /* mock restore */
        char *d = newdir("mock"); char dp[400], ap[400];
        CHECK(hv_g7_pair_begin(d, hv_install_disk_create, note, sizeof note) == 0, "mock: pair made");
        snprintf(dp, sizeof dp, "%s/%s", d, HV_INSTALL_DISK_NAME); snprintf(ap, sizeof ap, "%s/%s", d, HV_G7_AUX_NAME);
        HvDisk disk, aux; Tally td = {0}, ta = {0};
        CHECK(hv_disk_open(&disk, dp, 0, 0) == 0 && hv_disk_open(&aux, ap, 16384, 0) == 0, "both opened read-write (the mock stands in for the G7 runner)");
        for (int i = 0; i < 16; i++) mock_write(&disk, (uint64_t)i * (4u << 20), 1u << 20, (unsigned char)(i + 1), &td);
        mock_write(&aux, 16u << 20, 4096, 0x5a, &ta);          /* "aux sector 32768" */
        mock_write(&aux, 0, 512, 0x33, &ta);
        uint64_t c1, r1, c2, r2; hv_disk_write_stats(&disk, &c1, &r1); hv_disk_write_stats(&aux, &c2, &r2);
        CHECK(td.ok == 16 && td.failed == 0 && c1 == 16 && r1 == 0, "normal run: 16 disk writes, 0 failed, 0 refused");
        CHECK(ta.ok == 2 && ta.failed == 0 && c2 == 2 && r2 == 0, "normal run: 2 AUX writes, 0 failed, 0 refused");
        int allgood = 1; for (int i = 0; i < 16; i++) if (readback_ok(&disk, (uint64_t)i * (4u << 20), 1u << 20, (unsigned char)(i + 1)) != (1u << 20)) allgood = 0;
        CHECK(allgood && readback_ok(&aux, 16u << 20, 4096, 0x5a) == 4096 && readback_ok(&aux, 0, 512, 0x33) == 512, "read-back finds exactly the bytes that were reported written");
        hv_disk_flush(&disk); hv_disk_flush(&aux);
        CHECK(!same_file(d, HV_G7_AUX_NAME, HV_G7_AUX_INITIAL_NAME), "the AUX now differs from its initial copy (the copy kept the old state)");
        struct stat st; stat(dp, &st);
        CHECK((uint64_t)st.st_blocks * 512 >= 16u << 20 && (uint64_t)st.st_size == HV_INSTALL_DISK_BYTES, "disk: data allocated, size unchanged at 128 GiB");

        /* out of space / partial write: the file-size limit stands in for a full disk */
        struct rlimit old, lim;
        getrlimit(RLIMIT_FSIZE, &old); lim = old; lim.rlim_cur = (rlim_t)(64u << 20) + (512u << 10); setrlimit(RLIMIT_FSIZE, &lim);
        Tally tf = {0};
        mock_write(&disk, (64u << 20) - (1u << 20), 1u << 20, 0x71, &tf);    /* fits */
        mock_write(&disk, (64u << 20), 1u << 20, 0x72, &tf);                 /* crosses the limit: 512 KiB land, then the error */
        mock_write(&disk, (96u << 20), 1u << 20, 0x73, &tf);                 /* wholly past it */
        setrlimit(RLIMIT_FSIZE, &old);
        uint64_t c3, r3; hv_disk_write_stats(&disk, &c3, &r3);
        CHECK(tf.req == 3 && tf.ok == 1 && tf.failed == 2, "out of space: 1 ok, 2 failed (the caller sees the errors)");
        CHECK(r3 == 0, "the refused counter stays 0: these were I/O errors, not refusals (a counter alone would say 'nothing refused')");
        size_t part = readback_ok(&disk, (64u << 20), 1u << 20, 0x72);
        CHECK(part == (512u << 10), "partial write: the request reported FAILED yet %zu bytes of it are on the disk (found only by reading back)", part);
        CHECK(readback_ok(&disk, (96u << 20), 4096, 0x73) == 0, "the write wholly past the limit left nothing");
        CHECK(tf.bytes_ok == (1u << 20), "bytes counted as written = bytes of the successful request only");

        /* interruption: the machine stops here; nothing finishes the pair */
        hv_disk_close(&disk); hv_disk_close(&aux);
        CHECK(hv_g7_pair_state(d, note, sizeof note) == HV_G7_PAIR_IN_PROGRESS, "after the interruption the pair is still in_progress (%s)", note);
        CHECK(hv_g7_pair_finish(d, note, sizeof note) == 0 && hv_g7_pair_state(d, NULL, 0) == HV_G7_PAIR_DONE, "(the API cannot tell a confirmed restore from an interrupted one: only the caller that confirmed it may call finish)");
    }
    {   /* interruption and restart on a fresh directory, never finished */
        char *d = newdir("restart"), *ip = newdir("restart_ipsw"), *ec = newdir("restart_ecid"); good_inputs(d, ip, ec); in = input(d, ip, ec);
        char ap[400]; HvDisk aux; Tally ta = {0};
        CHECK(hv_g7_pair_begin(d, hv_install_disk_create, note, sizeof note) == 0, "restart: pair made");
        snprintf(ap, sizeof ap, "%s/%s", d, HV_G7_AUX_NAME);
        CHECK(hv_disk_open(&aux, ap, 16384, 0) == 0, "AUX opened read-write");
        mock_write(&aux, 4096, 4096, 0x42, &ta);
        hv_disk_close(&aux);                                             /* the process "dies" here */
        CHECK(hv_g7_preflight(&in, &rep) == -1 && has(&rep, "state", HV_G7_BLOCK), "restart: preflight blocks the half-written pair");
        CHECK(hv_g7_pair_begin(d, hv_install_disk_create, note, sizeof note) == -1, "restart: begin refuses to reuse or overwrite it");
        CHECK(hv_g7_pair_quarantine(d, "run1", note, sizeof note) == 4, "restart: the pair is moved aside as a unit");
        char q[400]; snprintf(q, sizeof q, "%s/%srun1", d, HV_G7_QUARANTINE_PREFIX);
        char kp[400]; snprintf(kp, sizeof kp, "%s/%s", q, HV_G7_AUX_NAME);
        HvDisk k; CHECK(hv_disk_open(&k, kp, 16384, 1) == 0 && readback_ok(&k, 4096, 4096, 0x42) == 4096, "restart: the half-written AUX is kept with its data for inspection");
        CHECK(same_file(q, HV_G7_AUX_INITIAL_NAME, HV_G7_AUX_INITIAL_NAME) && !same_file(q, HV_G7_AUX_NAME, HV_G7_AUX_INITIAL_NAME), "restart: the initial copy still holds the pre-restore state");
        CHECK(hv_g7_preflight(&in, &rep) == 0 && hv_g7_pair_begin(d, hv_install_disk_create, note, sizeof note) == 0, "restart: after the quarantine a new first run is possible (only by a new decision)");
    }
    {   /* the existing read-only contract is unchanged */
        char *d = newdir("ro"); char ap[400]; snprintf(ap, sizeof ap, "%s/f", d);
        put(d, "f", "0123456789abcdef", 16);
        HvDisk r; uint64_t calls, refused; unsigned char b[4] = {1, 2, 3, 4};
        CHECK(hv_disk_open(&r, ap, 0, 1) == 0 && hv_disk_write(&r, 0, b, 4) == -1, "the read-only open still refuses a write (existing contract unchanged)");
        hv_disk_write_stats(&r, &calls, &refused);
        CHECK(calls == 1 && refused == 1, "...and counts it as refused");
    }

    printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", T); (void)!system(cmd);
    return g_fail ? 1 : 0;
}
