/* host/runner_g7_core.c with a RECORDER in place of the machine and the restore. Built twice by tools/g7-preflight-test.sh:
     without HV_G7_MOCK_EXECUTION (what ships): a valid request ends at the switch, no call, nothing changed;
     with it (this test only): the full order pair -> machine -> restore runs against the recorder, and every refusal still happens before any call. Linux. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../host/disk.h"
#include "../host/disk_policy.h"
#include "../host/runner_g7_core.h"
#include "../host/runner_vm_core.h"

int hv_knob[HV_K_COUNT];

static int g_fail;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

#define ECID 0x0013579bdf024681ULL
#define ECID_HEX "13579bdf024681"

static struct { int pair, vm, restore, seq; int order_pair, order_vm, order_restore; HvVmConfig cfg; char disk[400], aux[400]; int cfg_runner_ok, ro; int fail_pair, fail_vm, fail_restore; const char *tamper_dir; int wrong_ecid; } rec;
static void rec_reset(void) { memset(&rec, 0, sizeof rec); }

static int api_pair(const char *dir, uint64_t ecid, char *note, int cap) {
    rec.pair++; rec.order_pair = ++rec.seq;
    if (rec.fail_pair) { snprintf(note, (size_t)cap, "injected"); return -1; }
    int r = hv_g7_pair_begin(dir, rec.wrong_ecid ? ecid + 1 : ecid, hv_install_disk_create, note, cap);
    if (r == 0 && rec.tamper_dir) { char p[400]; snprintf(p, sizeof p, "%s/%s", dir, HV_G7_AUX_NAME); int fd = open(p, O_WRONLY); if (fd >= 0) { (void)!pwrite(fd, "X", 1, 1u << 20); close(fd); } }
    return r;
}
static int api_vm(const HvVmConfig *c, char *err, size_t n) {
    char why[100];
    rec.vm++; rec.order_vm = ++rec.seq;
    rec.cfg = *c;
    snprintf(rec.disk, sizeof rec.disk, "%s", c->disk); snprintf(rec.aux, sizeof rec.aux, "%s", c->aux);
    rec.cfg.disk = rec.disk; rec.cfg.aux = rec.aux;
    rec.ro = c->readonly_storage;
    rec.cfg_runner_ok = hv_runner_config_ok(c, why, sizeof why) == 0;
    if (rec.fail_vm) { snprintf(err, n, "injected"); return -1; }
    return 0;
}
static int api_restore(void) { rec.restore++; rec.order_restore = ++rec.seq; return rec.fail_restore ? -1 : 0; }
static const HvG7Api API = {api_pair, api_vm, api_restore};

static char T[200];
static char G_ROM[260];
static void make_rom(const char *path, int flip) {
    FILE *f = fopen(path, "wb");
    for (unsigned long i = 0; f && i < HV_G7_ROM_BYTES; i++) { unsigned char b = (unsigned char)(i * 31 + 7); if (flip && i == 1000) b ^= 1; fputc(b, f); }
    if (f) fclose(f);
}
static int cnt;
static char *mk(const char *tag) { static char d[64][260]; char *p = d[cnt++ % 64]; snprintf(p, 260, "%s/%s", T, tag); mkdir(p, 0700); chmod(p, 0700); return p; }
static void put(const char *dir, const char *name, const char *s) { char p[400]; snprintf(p, sizeof p, "%s/%s", dir, name); int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600); if (fd >= 0) { (void)!write(fd, s, strlen(s)); close(fd); } }
static void sparse(const char *dir, const char *name, unsigned long long n) { char p[400]; snprintf(p, sizeof p, "%s/%s", dir, name); int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600); if (fd >= 0) { (void)!ftruncate(fd, (off_t)n); close(fd); } }
static void snap(const char *dir, char *out, size_t cap) {
    DIR *d = opendir(dir); struct dirent *e; out[0] = 0; size_t n = 0;
    while (d && (e = readdir(d))) { struct stat st; char p[400]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name); if (lstat(p, &st) == 0 && n < cap - 120) n += (size_t)snprintf(out + n, cap - n, "%s:%lld:%lld.%ld;", e->d_name, (long long)st.st_size, (long long)st.st_mtim.tv_sec, st.st_mtim.tv_nsec); }
    if (d) closedir(d);
}
/* readdir order is not stable across runs of the same directory only if it changes: compare the sorted sets via a second call below, same process, same dir */

typedef struct { char *dir, *ipsw, *ecid; HvG7Request r; } Set;
static Set good(const char *tag) {
    Set s; char t[120]; static int uniq;
    snprintf(t, sizeof t, "%s%d_d", tag, uniq++); s.dir = mk(t);
    snprintf(t, sizeof t, "%s%d_i", tag, uniq); s.ipsw = mk(t);
    snprintf(t, sizeof t, "%s%d_e", tag, uniq); s.ecid = mk(t);
    sparse(s.ipsw, HV_G7_IPSW_NAME, HV_G7_IPSW_BYTES);
    put(s.ecid, HV_G7_ECID_FILE, "ecid=0x" ECID_HEX "\n");
    s.r = (HvG7Request){.dir = s.dir, .ipsw_dir = s.ipsw, .ecid_dir = s.ecid, .rom = G_ROM, .ecid = ECID, .ram_mb = 2048, .ncpus = 2, .jit_mb = 64};
    return s;
}
static int calls(void) { return rec.pair + rec.vm + rec.restore; }

int main(void) {
    snprintf(T, sizeof T, "/tmp/hvg7r-XXXXXX");
    if (!mkdtemp(T)) return 2;
    HvG7Result res; char before[8000], after[8000];
    snprintf(G_ROM, sizeof G_ROM, "%s/rom.bin", T);
    make_rom(G_ROM, 0);

    printf("--- config construction\n");
    {
        Set s = good("cfg"); HvVmConfig c; char dk[300], ax[300], why[120];
        CHECK(hv_g7_make_config(&c, &s.r, dk, sizeof dk, ax, sizeof ax) == 0, "make_config fills a configuration");
        CHECK(c.readonly_storage == 0 && c.installer == 1 && c.seed == 0 && c.ecid == ECID, "writable storage, installer, seed 0, the request's identity");
        CHECK(hv_g7_config_ok(&c, s.dir, ECID, why, sizeof why) == 0, "hv_g7_config_ok accepts it");
        CHECK(hv_runner_config_ok(&c, why, sizeof why) == -1, "hv_runner_config_ok (G0-G6, [A]) refuses it: %s", why);
        char tiny[8];
        CHECK(hv_g7_make_config(&c, &s.r, tiny, sizeof tiny, ax, sizeof ax) == -1, "a path that does not fit is an error, not a truncated path");
    }

    printf("--- refusals happen before any call\n");
    {
        Set s; HvG7Request r;
#define REFUSE(label, setup, want_stage) do { rec_reset(); s = good("rf"); r = s.r; setup; snap(s.dir, before, sizeof before); \
            int rc = hv_g7_run(&r, &API, &res); snap(s.dir, after, sizeof after); \
            CHECK(rc == -1 && res.verdict == HV_G7_REFUSED && res.stage == (want_stage) && calls() == 0 && !strcmp(before, after), "refused at stage %d with no call and nothing changed: %s (%s)", (int)(want_stage), label, res.reason); } while (0)
        REFUSE("no ECID in the request", r.ecid = 0, HV_G7_STAGE_CONFIG);
        REFUSE("RAM below the installer's minimum", r.ram_mb = 512, HV_G7_STAGE_CONFIG);
        REFUSE("too many CPUs", r.ncpus = 9, HV_G7_STAGE_CONFIG);
        REFUSE("no ROM path", r.rom = NULL, HV_G7_STAGE_CONFIG);
        REFUSE("no directory", r.dir = NULL, HV_G7_STAGE_CONFIG);
        REFUSE("ECID differs from vm_ecid.txt", r.ecid = ECID + 1, HV_G7_STAGE_PREFLIGHT);
        REFUSE("a ROM that is not the expected one", { static char rp[300]; snprintf(rp, sizeof rp, "%s/other_rom.bin", T); make_rom(rp, 1); r.rom = rp; }, HV_G7_STAGE_PREFLIGHT);
        REFUSE("IPSW missing", { char p[400]; snprintf(p, sizeof p, "%s/%s", s.ipsw, HV_G7_IPSW_NAME); unlink(p); }, HV_G7_STAGE_PREFLIGHT);
        REFUSE("a disk already there", sparse(s.dir, HV_INSTALL_DISK_NAME, HV_INSTALL_DISK_BYTES), HV_G7_STAGE_PREFLIGHT);
        REFUSE("an AUX already there", sparse(s.dir, HV_G7_AUX_NAME, HV_G7_AUX_BYTES), HV_G7_STAGE_PREFLIGHT);
        REFUSE("an interrupted pair's state file", put(s.dir, HV_G7_STATE_NAME, "state=in_progress\n"), HV_G7_STAGE_PREFLIGHT);
        REFUSE("a group-writable directory", chmod(s.dir, 0777), HV_G7_STAGE_PREFLIGHT);
        CHECK(hv_g7_run(NULL, &API, &res) == -1 && res.verdict == HV_G7_REFUSED, "a NULL request is refused");
    }

    printf("--- a valid request\n");
    {
        Set s = good("ok"); rec_reset();
        snap(s.dir, before, sizeof before);
        int rc = hv_g7_run(&s.r, &API, &res);
#if !HV_G7_EXECUTION_ENABLED
        snap(s.dir, after, sizeof after);
        CHECK(rc == -1 && res.verdict == HV_G7_BLOCKED_BY_SWITCH && res.stage == HV_G7_STAGE_SWITCH, "shipped build: a valid request stops at the switch (%s)", res.reason);
        CHECK(calls() == 0, "no machine, no restore, no pair call");
        CHECK(!strcmp(before, after), "the directory is exactly as it was: no disk, AUX or state file");
        CHECK(hv_g7_run(&s.r, NULL, &res) == -1 && res.verdict == HV_G7_BLOCKED_BY_SWITCH, "the same with no execution table at all");
#else
        CHECK(rc == 0 && res.verdict == HV_G7_RAN && res.stage == HV_G7_STAGE_DONE, "mock build: the run goes through (%s)", res.reason);
        CHECK(rec.pair == 1 && rec.vm == 1 && rec.restore == 1, "one pair call, one machine, one restore");
        CHECK(rec.order_pair < rec.order_vm && rec.order_vm < rec.order_restore, "in that order");
        char want[400];
        snprintf(want, sizeof want, "%s/%s", s.dir, HV_INSTALL_DISK_NAME);
        CHECK(rec.ro == 0 && !strcmp(rec.cfg.disk, want), "the machine got writable storage and exactly dir/macos_disk.img");
        snprintf(want, sizeof want, "%s/%s", s.dir, HV_G7_AUX_NAME);
        char why[100];
        CHECK(!strcmp(rec.cfg.aux, want) && hv_g7_config_ok(&rec.cfg, s.dir, ECID, why, sizeof why) == 0, "...exactly dir/macos_aux.img, and that structure passes the G7 validator");
        CHECK(rec.cfg_runner_ok == 0, "...and does NOT pass the G0-G6 runner validator");
        CHECK(hv_g7_pair_state(s.dir, NULL, 0) == HV_G7_PAIR_IN_PROGRESS, "the pair is in_progress: nothing in the runner marks it done");
        CHECK(hv_g7_pair_verify_initial(s.dir, NULL, 0) == 0, "the pair the run made is intact");
        /* a second run must be refused: the directory is no longer clean */
        rec_reset();
        CHECK(hv_g7_run(&s.r, &API, &res) == -1 && res.verdict == HV_G7_REFUSED && res.stage == HV_G7_STAGE_PREFLIGHT && calls() == 0, "a second run is refused by the preflight, with no call");
        /* failures */
        /* a pair that was damaged between its creation and the machine must never reach the machine */
        Set bad = good("badpair"); rec_reset();
        rec.tamper_dir = bad.dir;
        CHECK(hv_g7_run(&bad.r, &API, &res) == -1 && res.verdict == HV_G7_FAILED && res.stage == HV_G7_STAGE_PAIR && rec.vm == 0 && rec.restore == 0, "a pair whose AUX changed after it was made never reaches the machine (%s)", res.reason);
        /* a pair tied to another ECID never reaches the machine, even though it is otherwise intact */
        Set we = good("wrongecid"); rec_reset(); rec.wrong_ecid = 1;
        CHECK(hv_g7_run(&we.r, &API, &res) == -1 && res.verdict == HV_G7_FAILED && res.stage == HV_G7_STAGE_PAIR && rec.vm == 0 && rec.restore == 0 && !strstr(res.reason, ECID_HEX), "a pair made for another ECID never reaches the machine (%s)", res.reason);
        Set f1 = good("f1"); rec_reset(); rec.fail_pair = 1;
        CHECK(hv_g7_run(&f1.r, &API, &res) == -1 && res.verdict == HV_G7_FAILED && res.stage == HV_G7_STAGE_PAIR && rec.vm == 0 && rec.restore == 0, "pair failure: no machine, no restore");
        Set f2 = good("f2"); rec_reset(); rec.fail_vm = 1;
        CHECK(hv_g7_run(&f2.r, &API, &res) == -1 && res.verdict == HV_G7_FAILED && res.stage == HV_G7_STAGE_VM && rec.restore == 0, "machine failure: no restore");
        CHECK(hv_g7_pair_state(f2.dir, NULL, 0) == HV_G7_PAIR_IN_PROGRESS, "...the pair is kept (in_progress), not deleted or reset");
        rec_reset();
        CHECK(hv_g7_run(&f2.r, &API, &res) == -1 && res.stage == HV_G7_STAGE_PREFLIGHT && calls() == 0, "...and it blocks the next run until it is quarantined");
        char note[200];
        CHECK(hv_g7_pair_quarantine(f2.dir, "fail1", note, sizeof note) == 4, "quarantine sets the failed pair aside");
        rec_reset();
        CHECK(hv_g7_run(&f2.r, &API, &res) == 0, "after that a new run is possible");
        Set f3 = good("f3"); rec_reset(); rec.fail_restore = 1;
        CHECK(hv_g7_run(&f3.r, &API, &res) == -1 && res.stage == HV_G7_STAGE_RESTORE && res.verdict == HV_G7_FAILED && hv_g7_pair_state(f3.dir, NULL, 0) == HV_G7_PAIR_IN_PROGRESS, "restore start failure: reported, pair kept in_progress");
        rec_reset();
        CHECK(hv_g7_run(&s.r, NULL, &res) == -1 && res.verdict == HV_G7_REFUSED, "an execution run without an API table is refused");
        Set g = good("incomplete"); rec_reset();
        HvG7Api half = {api_pair, NULL, api_restore};
        CHECK(hv_g7_run(&g.r, &half, &res) == -1 && res.verdict == HV_G7_REFUSED && calls() == 0 && hv_g7_pair_state(g.dir, NULL, 0) == HV_G7_PAIR_NONE, "an incomplete table is refused before the pair is made");
#endif
    }
    printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", T); (void)!system(cmd);
    return g_fail ? 1 : 0;
}
