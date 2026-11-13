/* C1-2: host/runner_vm_core.c with a RECORDER where the virtual machine would be. Nothing here links host/vm.c, so it runs on any Linux (x86 too) and under ASan / UBSan / TSan.
   What this shows: how commands are checked and turned into calls (and counted); that the machine's answers about the picture and the pointer, valid, absent and broken, reach
   the shared file as they should; that a closed picture path never asks the machine again; the safety settings structure and its refusals; the HV_* environment scrub; and,
   for the read-only claim, the disk layer's own refusal of a write.
   What it does NOT show: anything about a real machine or a real guest. A recorder answering "picture present" proves the path from the answer to the shared file, not that
   a guest ever produces such an answer (tests/test_c1vm_proc.c runs the real runner with a stand-in ROM: no picture there). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "disk.h"
#include "display_buf.h"
#include "frame_shm.h"
#include "machine.h"
#include "runner_vm_core.h"

int hv_knob[HV_K_COUNT];   /* host/disk.c reads this engine setting; the engine itself is not linked here */

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the recorder ---- */
enum { E_KEY = 1, E_MOVE, E_BUTTON, E_WHEEL, E_HWHEEL };
typedef struct { int kind, a, b; } Ev;
static Ev g_ev[40000];
static int g_nev;
static void rec(int k, int a, int b) { if (g_nev < (int)(sizeof g_ev / sizeof g_ev[0])) { g_ev[g_nev].kind = k; g_ev[g_nev].a = a; g_ev[g_nev].b = b; } g_nev++; }
static void r_key(uint16_t c, int d) { rec(E_KEY, c, d); }
static void r_move(int dx, int dy) { rec(E_MOVE, dx, dy); }
static void r_button(int b, int d) { rec(E_BUTTON, b, d); }
static void r_wheel(int d) { rec(E_WHEEL, d, 0); }
static void r_hwheel(int d) { rec(E_HWHEEL, d, 0); }

enum { D_ABSENT, D_PRESENT, D_NULLPIX, D_ZEROW, D_ZEROH, D_SHORTSTRIDE, D_TOOWIDE, D_TOOBIG };
static int g_dmode = D_ABSENT;
static uint8_t *g_dpix;      /* the "guest's" framebuffer */
static uint32_t g_dw = 320, g_dh = 200, g_dstride = 320 * 4 + 64;
static int g_dcalls;
static int r_display(HvBootDisplay *d) {
    g_dcalls++;
    switch (g_dmode) {
    case D_ABSENT: return -1;
    case D_PRESENT: d->pixels = g_dpix; d->width = g_dw; d->height = g_dh; d->stride = g_dstride; return 0;
    case D_NULLPIX: d->pixels = NULL; d->width = g_dw; d->height = g_dh; d->stride = g_dstride; return 0;
    case D_ZEROW: d->pixels = g_dpix; d->width = 0; d->height = g_dh; d->stride = g_dstride; return 0;
    case D_ZEROH: d->pixels = g_dpix; d->width = g_dw; d->height = 0; d->stride = g_dstride; return 0;
    case D_SHORTSTRIDE: d->pixels = g_dpix; d->width = g_dw; d->height = g_dh; d->stride = g_dw * 4 - 4; return 0;
    case D_TOOWIDE: d->pixels = g_dpix; d->width = HV_DISP_MAX_DIM + 1; d->height = 8; d->stride = (HV_DISP_MAX_DIM + 1) * 4; return 0;
    case D_TOOBIG: d->pixels = g_dpix; d->width = 4096; d->height = 4096; d->stride = 4096 * 4; return 0;   /* 64 MB: over the shared file's room */
    }
    return -1;
}
enum { P_ABSENT, P_VALID, P_BIGX, P_NEGY };
static int g_pmode = P_ABSENT, g_px = 123, g_py = 45;
static int r_pointer(int *x, int *y) {
    switch (g_pmode) {
    case P_ABSENT: return -1;
    case P_VALID: *x = g_px; *y = g_py; return 0;
    case P_BIGX: *x = 70000; *y = 5; return 0;
    case P_NEGY: *x = 5; *y = -3; return 0;
    }
    return -1;
}
static const HvRunnerApi API = {r_display, r_pointer, r_key, r_move, r_button, r_wheel, r_hwheel};

static char g_dir[128];
static HvShm *mk_shm(const char *name, HvShm **app) {
    char p[256]; snprintf(p, sizeof p, "%s/%s", g_dir, name);
    HvShm *s = NULL;
    if (hv_shm_create(p, &s) != 0) { printf("FAIL cannot create %s\n", p); exit(1); }
    if (app && hv_shm_open(p, app) != 0) { printf("FAIL cannot open %s\n", p); exit(1); }
    return s;
}
static HvRunnerStats stats(HvRunner *r) { HvRunnerStats s; hv_runner_stats(r, &s); return s; }

/* ---- commands ---- */
static void test_commands(void) {
    HvShm *app, *rs = mk_shm("cmds.shm", &app);
    HvRunner *r = hv_runner_create(&API, rs);
    CHECK(r != NULL, "runner created");
    g_nev = 0;
    /* not ready: valid commands are popped and counted as skipped; nothing reaches the machine */
    hv_shm_push_cmd(app, HV_SHM_CMD_MOVE, 5, 5); hv_shm_push_cmd(app, HV_SHM_CMD_KEY, 30, 1);
    CHECK(hv_runner_pump_cmds(r) == 2, "two popped while not ready");
    HvRunnerStats s = stats(r);
    CHECK(s.cmds_popped == 2 && s.cmds_skipped == 2 && s.cmds_dispatched == 0 && s.calls_made == 0 && g_nev == 0, "not ready: popped %llu skipped %llu calls %llu events %d", (unsigned long long)s.cmds_popped, (unsigned long long)s.cmds_skipped, (unsigned long long)s.calls_made, g_nev);
    hv_runner_set_ready(r, 1);
    /* every kind, in order */
    hv_shm_push_cmd(app, HV_SHM_CMD_KEY, 30, 1); hv_shm_push_cmd(app, HV_SHM_CMD_KEY, 30, 0);
    hv_shm_push_cmd(app, HV_SHM_CMD_MOVE, -7, 9); hv_shm_push_cmd(app, HV_SHM_CMD_MOVE, 0, 0);
    hv_shm_push_cmd(app, HV_SHM_CMD_BUTTON, 0, 1); hv_shm_push_cmd(app, HV_SHM_CMD_BUTTON, 2, 0);
    hv_shm_push_cmd(app, HV_SHM_CMD_WHEEL, -3, 0); hv_shm_push_cmd(app, HV_SHM_CMD_HWHEEL, 4, 0);
    CHECK(hv_runner_pump_cmds(r) == 8, "eight popped");
    static const Ev want[8] = {{E_KEY, 30, 1}, {E_KEY, 30, 0}, {E_MOVE, -7, 9}, {E_MOVE, 0, 0}, {E_BUTTON, 0, 1}, {E_BUTTON, 2, 0}, {E_WHEEL, -3, 0}, {E_HWHEEL, 4, 0}};
    int same = g_nev == 8;
    for (int i = 0; same && i < 8; i++) same = g_ev[i].kind == want[i].kind && g_ev[i].a == want[i].a && g_ev[i].b == want[i].b;
    CHECK(same, "the eight calls are the eight commands, same order and arguments (%d events)", g_nev);
    /* a button's "down" is any non-zero */
    hv_shm_push_cmd(app, HV_SHM_CMD_BUTTON, 1, 7); hv_runner_pump_cmds(r);
    CHECK(g_nev == 9 && g_ev[8].kind == E_BUTTON && g_ev[8].a == 1 && g_ev[8].b == 1, "a non-zero down is passed as 1");
    /* invalid commands: rejected, no call */
    int before = g_nev;
    struct { uint32_t t; int a, b; } bad[] = {{HV_SHM_CMD_KEY, 0, 1}, {HV_SHM_CMD_KEY, 0x300, 1}, {HV_SHM_CMD_KEY, -1, 1}, {HV_SHM_CMD_BUTTON, 3, 1}, {HV_SHM_CMD_BUTTON, -1, 1},
                                              {HV_SHM_CMD_WHEEL, 2000, 0}, {HV_SHM_CMD_HWHEEL, -2000, 0}, {HV_SHM_CMD_MOVE, 40000, 0}, {HV_SHM_CMD_MOVE, 0, -40000}, {99, 0, 0}, {0, 0, 0}};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) hv_shm_push_cmd(app, bad[i].t, bad[i].a, bad[i].b);
    hv_runner_pump_cmds(r);
    s = stats(r);
    CHECK(g_nev == before && s.cmds_rejected == sizeof bad / sizeof bad[0], "invalid commands: rejected %llu, no call made", (unsigned long long)s.cmds_rejected);
    CHECK(s.cmds_popped == s.cmds_dispatched + s.cmds_skipped + s.cmds_rejected, "popped = dispatched + skipped + rejected (%llu = %llu + %llu + %llu)",
          (unsigned long long)s.cmds_popped, (unsigned long long)s.cmds_dispatched, (unsigned long long)s.cmds_skipped, (unsigned long long)s.cmds_rejected);
    CHECK(s.calls_made == (uint64_t)g_nev && s.calls_made == s.cmds_dispatched, "calls made = calls the recorder saw = %d", g_nev);
    /* an invalid command is rejected even while not ready, and a stop (ready off) skips again */
    hv_runner_set_ready(r, 0);
    hv_shm_push_cmd(app, 99, 0, 0); hv_shm_push_cmd(app, HV_SHM_CMD_MOVE, 1, 1); hv_runner_pump_cmds(r);
    HvRunnerStats s2 = stats(r);
    CHECK(s2.cmds_rejected == s.cmds_rejected + 1 && s2.cmds_skipped == s.cmds_skipped + 1 && s2.calls_made == s.calls_made, "after the stop: bad one rejected, good one skipped, no call");
    CHECK(hv_runner_pump_cmds(r) == 0, "nothing waiting: 0");
    /* the ring's own count of what was popped matches ours */
    HvShmStats ss; hv_shm_stats(app, &ss);
    CHECK(ss.cmds_popped == s2.cmds_popped && ss.cmds_pushed == ss.cmds_popped, "ring popped %llu = runner popped %llu", (unsigned long long)ss.cmds_popped, (unsigned long long)s2.cmds_popped);
    hv_runner_close_display(r); hv_runner_destroy(r); hv_shm_close(rs); hv_shm_close(app);
}

/* ---- picture ---- */
static int frame_equals_guest(const uint8_t *tight) {
    for (uint32_t y = 0; y < g_dh; y++) if (memcmp(tight + (size_t)y * g_dw * 4, g_dpix + (size_t)y * g_dstride, (size_t)g_dw * 4)) return 0;
    return 1;
}
static void test_picture(void) {
    HvShm *app, *rs = mk_shm("pic.shm", &app);
    HvRunner *r = hv_runner_create(&API, rs);
    static uint8_t out[4 * 1024 * 1024];
    uint64_t seen = 0; uint32_t w = 0, h = 0;
    g_dpix = malloc((size_t)g_dstride * g_dh);
    hv_testpattern(g_dpix, g_dw, g_dh, g_dstride, 3, 10, 10);
    /* nothing yet */
    g_dmode = D_ABSENT; g_dcalls = 0;
    CHECK(hv_runner_pump_frame(r) == HV_DISP_NONE, "absent: no picture");
    HvRunnerStats s = stats(r);
    CHECK(s.display_absent == 1 && s.display_present == 0 && s.frames_published == 0 && s.queries_made == 1 && g_dcalls == 1, "absent counted: absent %llu queries %llu", (unsigned long long)s.display_absent, (unsigned long long)s.queries_made);
    CHECK(hv_shm_read_frame(app, &seen, out, sizeof out, &w, &h) == -1, "the shared file holds no picture");
    /* present, with a row length longer than the row */
    g_dmode = D_PRESENT;
    CHECK(hv_runner_pump_frame(r) == HV_DISP_NEW, "present: a new picture");
    CHECK(hv_shm_read_frame(app, &seen, out, sizeof out, &w, &h) == 1 && w == g_dw && h == g_dh, "the app reads a %ux%u picture", w, h);
    CHECK(frame_equals_guest(out), "the picture in the shared file equals the guest's, rows tightly packed");
    s = stats(r);
    CHECK(s.display_present == 1 && s.frames_published == 1 && s.call_errors == 0, "present counted");
    CHECK(hv_shm_read_frame(app, &seen, out, sizeof out, &w, &h) == 0, "nothing newer until the runner publishes again");
    /* the guest draws: the next pull carries it */
    hv_testpattern(g_dpix, g_dw, g_dh, g_dstride, 4, 50, 60);
    CHECK(hv_runner_pump_frame(r) == HV_DISP_NEW && hv_shm_read_frame(app, &seen, out, sizeof out, &w, &h) == 1 && frame_equals_guest(out), "a changed picture arrives");
    /* answers that break the contract: counted, nothing published, the old picture stays */
    int modes[] = {D_NULLPIX, D_ZEROW, D_ZEROH, D_SHORTSTRIDE, D_TOOWIDE, D_TOOBIG};
    const char *names[] = {"no pixels", "zero width", "zero height", "row shorter than the width", "wider than the limit", "bigger than the shared file's room"};
    for (int i = 0; i < 6; i++) {
        HvRunnerStats a = stats(r);
        g_dmode = modes[i];
        int rc = hv_runner_pump_frame(r);
        HvRunnerStats b = stats(r);
        CHECK(rc == HV_DISP_NONE && b.call_errors == a.call_errors + 1 && b.frames_published == a.frames_published && b.display_present == a.display_present, "broken answer (%s): counted as an error, not published", names[i]);
        CHECK(hv_shm_read_frame(app, &seen, out, sizeof out, &w, &h) == 0, "broken answer (%s): the old picture stays, nothing newer", names[i]);
    }
    /* present, then absent again: the old picture stays (the app keeps showing it) */
    g_dmode = D_ABSENT;
    CHECK(hv_runner_pump_frame(r) == HV_DISP_NONE && hv_shm_read_frame(app, &seen, out, sizeof out, &w, &h) == 0, "absent after present: nothing newer, picture stays");
    /* closed: the machine is not asked again */
    g_dmode = D_PRESENT; g_dcalls = 0;
    HvRunnerStats a = stats(r);
    hv_runner_close_display(r); hv_runner_close_display(r);
    CHECK(hv_runner_pump_frame(r) == HV_DISP_CLOSED && g_dcalls == 0, "after close: closed, and the machine was not asked (%d calls)", g_dcalls);
    HvRunnerStats b = stats(r);
    CHECK(b.queries_made == a.queries_made, "after close: no query counted");
    free(g_dpix); hv_runner_destroy(r); hv_shm_close(rs); hv_shm_close(app);
}

/* ---- pointer ---- */
static void test_pointer(void) {
    HvShm *app, *rs = mk_shm("ptr.shm", &app);
    HvRunner *r = hv_runner_create(&API, rs);
    int x = -9, y = -9;
    g_pmode = P_ABSENT;
    CHECK(hv_runner_pump_pointer(r) == 0 && hv_shm_get_pointer(app, &x, &y) == 0 && x == -9 && y == -9, "absent: the app sees no pointer");
    g_pmode = P_VALID; g_px = 123; g_py = 45;
    CHECK(hv_runner_pump_pointer(r) == 1 && hv_shm_get_pointer(app, &x, &y) == 1 && x == 123 && y == 45, "valid: the app sees 123,45 (%d,%d)", x, y);
    g_px = 0; g_py = 65535;
    CHECK(hv_runner_pump_pointer(r) == 1 && hv_shm_get_pointer(app, &x, &y) == 1 && x == 0 && y == 65535, "the edges of the range are valid");
    g_pmode = P_ABSENT;
    CHECK(hv_runner_pump_pointer(r) == 0 && hv_shm_get_pointer(app, &x, &y) == 0, "valid then absent: cleared");
    HvRunnerStats a = stats(r);
    g_pmode = P_VALID; g_px = 7; g_py = 8; hv_runner_pump_pointer(r);
    g_pmode = P_BIGX;
    CHECK(hv_runner_pump_pointer(r) == -1 && hv_shm_get_pointer(app, &x, &y) == 0, "x outside 0..65535: error, not valid");
    g_pmode = P_VALID; g_px = 7; g_py = 8; hv_runner_pump_pointer(r);
    g_pmode = P_NEGY;
    CHECK(hv_runner_pump_pointer(r) == -1 && hv_shm_get_pointer(app, &x, &y) == 0, "negative y: error, not valid");
    HvRunnerStats b = stats(r);
    CHECK(b.call_errors == a.call_errors + 2 && b.pointer_valid == a.pointer_valid + 2 && b.pointer_absent == a.pointer_absent, "counts: errors +2, valid +2");
    CHECK(b.queries_made == b.pointer_valid + b.pointer_absent + b.call_errors, "every pointer query is valid, absent or an error (%llu)", (unsigned long long)b.queries_made);
    hv_runner_close_display(r); hv_runner_destroy(r); hv_shm_close(rs); hv_shm_close(app);
}

/* ---- threads: the app pushes and reads while the runner pumps ---- */
static HvShm *t_app;
static atomic_int t_done, t_bad_frames;
static atomic_long t_reads;
static void *producer(void *a) { (void)a; for (int i = 0; i < 20000;) { int r = hv_shm_push_cmd(t_app, HV_SHM_CMD_MOVE, i, -i); if (r == 1) i++; else usleep(20); } atomic_store(&t_done, 1); return NULL; }
static void *reader(void *a) {
    (void)a;
    static uint8_t buf[320 * 200 * 4 + 16];
    uint64_t seen = 0; uint32_t w, h;
    while (!atomic_load(&t_done) || atomic_load(&t_reads) < 5) {
        if (hv_shm_read_frame(t_app, &seen, buf, sizeof buf, &w, &h) == 1) {
            atomic_fetch_add(&t_reads, 1);
            for (size_t i = 4; i < (size_t)w * h * 4; i++) if (buf[i] != buf[0]) { atomic_fetch_add(&t_bad_frames, 1); break; }   /* each guest frame is one value: a torn copy would mix two */
        } else usleep(100);
    }
    return NULL;
}
static void test_threads(void) {
    HvShm *app, *rs = mk_shm("thr.shm", &app);
    t_app = app;
    HvRunner *r = hv_runner_create(&API, rs);
    hv_runner_set_ready(r, 1);
    g_nev = 0; g_dmode = D_PRESENT; g_pmode = P_VALID; g_px = 1; g_py = 2;
    g_dw = 320; g_dh = 200; g_dstride = 320 * 4;
    g_dpix = malloc((size_t)g_dstride * g_dh);
    memset(g_dpix, 0, (size_t)g_dstride * g_dh);
    pthread_t p, rd;
    pthread_create(&p, NULL, producer, NULL); pthread_create(&rd, NULL, reader, NULL);
    long popped = 0; int v = 0;
    double t0 = (double)time(NULL);
    while (popped < 20000 && (double)time(NULL) - t0 < 60) {
        int n = hv_runner_pump_cmds(r);
        if (n > 0) popped += n;
        memset(g_dpix, ++v & 0xff, (size_t)g_dstride * g_dh);   /* the guest draws between pulls (never during one: same thread) */
        hv_runner_pump_frame(r);
        hv_runner_pump_pointer(r);
        if (n <= 0) usleep(200);
    }
    pthread_join(p, NULL);
    hv_runner_pump_cmds(r);
    hv_runner_pump_frame(r);
    pthread_join(rd, NULL);
    int ordered = g_nev == 20000;
    for (int i = 0; ordered && i < 20000; i++) ordered = g_ev[i].kind == E_MOVE && g_ev[i].a == i && g_ev[i].b == -i;
    HvRunnerStats s = stats(r);
    CHECK(popped >= 20000 && s.cmds_popped == 20000 && s.cmds_dispatched == 20000 && ordered, "20000 commands from another thread: popped %llu, dispatched %llu, in order %d", (unsigned long long)s.cmds_popped, (unsigned long long)s.cmds_dispatched, ordered);
    CHECK(atomic_load(&t_reads) >= 5 && atomic_load(&t_bad_frames) == 0, "the reader thread read %ld pictures, torn: %d", atomic_load(&t_reads), atomic_load(&t_bad_frames));
    CHECK(s.shm_errors == 0, "no lock timeout on the runner's side (%llu)", (unsigned long long)s.shm_errors);
    free(g_dpix);
    hv_runner_close_display(r); hv_runner_destroy(r); hv_shm_close(rs); hv_shm_close(app);
}

/* ---- the safety settings ---- */
static void test_config(void) {
    HvVmConfig c; char why[200], line[400];
    hv_runner_make_config(&c, "rom", "disk", "aux", 256, 2, 64);
    CHECK(hv_runner_config_ok(&c, why, sizeof why) == 0, "the made structure is accepted (%s)", why);
    CHECK(c.readonly_storage == 1 && c.no_net == 1 && c.installer == 0 && c.ecid == 0 && c.seed == 0 && c.jit_rx == NULL && c.jit_rw == NULL, "read-only, no network, no installer, no identity, no host JIT memory");
    CHECK(c.aux_offset == 16384 && c.jit_bytes == (size_t)64 << 20 && c.ram_mb == 256 && c.ncpus == 2, "sizes as asked");
    hv_runner_config_line(&c, line, sizeof line);
    CHECK(strstr(line, "readonly_storage=1") && strstr(line, "no_net=1") && strstr(line, "installer=0") && strstr(line, "identity=none") && !strstr(line, "rom") && !strstr(line, "disk"), "the printed line comes from the structure: %s", line);
    /* every unsafe change is refused, one at a time, on a copy of the good structure */
#define NEG(what, stmt, expect) do { HvVmConfig d = c; stmt; why[0] = 0; CHECK(hv_runner_config_ok(&d, why, sizeof why) == -1 && strstr(why, expect), "refused: %s (%s)", what, why); } while (0)
    NEG("storage writable", d.readonly_storage = 0, "read-only");
    NEG("network on", d.no_net = 0, "network");
    NEG("installer boot", d.installer = 1, "installer");
    NEG("an ECID", d.ecid = 1, "identity");
    NEG("a seed", d.seed = 1, "identity");
    NEG("host JIT memory", d.jit_rx = (void *)&d, "JIT memory");
    NEG("no ROM", d.rom = NULL, "path");
    NEG("empty disk path", d.disk = "", "path");
    NEG("no aux", d.aux = NULL, "path");
    NEG("RAM too small", d.ram_mb = 8, "RAM");
    NEG("RAM too large", d.ram_mb = 100000, "RAM");
    NEG("no CPU", d.ncpus = 0, "CPU");
    NEG("too many CPUs", d.ncpus = 99, "CPU");
    NEG("JIT too small", d.jit_bytes = 1, "JIT size");
    NEG("JIT too large", d.jit_bytes = (size_t)4 << 30, "JIT size");
    CHECK(hv_runner_config_ok(NULL, why, sizeof why) == -1, "no structure: refused");
    /* the made structure with silly sizes is caught by the check, not by make */
    hv_runner_make_config(&c, "r", "d", "a", 0, 0, 0);
    CHECK(hv_runner_config_ok(&c, why, sizeof why) == -1, "zero sizes: refused");
}

static int unset_fail_one(const char *n) { return !strcmp(n, "HV_FAIL") ? -1 : unsetenv(n); }          /* says no for one name */
static int unset_lie_one(const char *n) { return !strcmp(n, "HV_LIE") ? 0 : unsetenv(n); }              /* says yes and does nothing for one name */
static int unset_rc_but_removed(const char *n) { unsetenv(n); return -1; }                                // removes it and still reports an error
static int unset_fail_all(const char *n) { (void)n; return -1; }
static void clear_hv(void) { HvEnvOps real = {unsetenv}; hv_runner_scrub_env_ops(&real, NULL, 0, NULL, 0); }
static void test_env(void) {
    char names[256], left[256];
    setenv("HV_RUN_INSTALLER", "1,1", 1); setenv("HV_USB_ON_NORMAL", "1", 1); setenv("HV_RAM_FILE", "/nonexistent/SECRETVAL", 1); setenv("HV_VUSB_PROBE", "1", 1); setenv("HVX_NOT", "keep", 1); setenv("MYHV_OK", "keep", 1);
    int n = hv_runner_scrub_env(names, sizeof names, left, sizeof left);
    CHECK(n == 4, "four HV_* variables removed (%d)", n);
    CHECK(strstr(names, "HV_RUN_INSTALLER") && strstr(names, "HV_USB_ON_NORMAL") && strstr(names, "HV_RAM_FILE") && strstr(names, "HV_VUSB_PROBE") && !strchr(names, '=') && !strstr(names, "SECRETVAL"), "names listed, no values: %s", names);
    CHECK(!getenv("HV_RUN_INSTALLER") && !getenv("HV_USB_ON_NORMAL") && !getenv("HV_RAM_FILE") && !getenv("HV_VUSB_PROBE") && left[0] == 0, "they are gone, nothing left");
    CHECK(getenv("HVX_NOT") && getenv("MYHV_OK"), "names that only look similar stay (HVX_NOT, MYHV_OK)");
    CHECK(hv_runner_scrub_env(NULL, 0, NULL, 0) == 0, "second pass: nothing to remove, clean (0)");
    for (int i = 0; i < 300; i++) { char nm[32]; snprintf(nm, sizeof nm, "HV_MANY_%d", i); setenv(nm, "1", 1); }
    CHECK(hv_runner_scrub_env(NULL, 0, NULL, 0) == 300, "300 variables all removed");
    CHECK(!getenv("HV_MANY_299") && !getenv("HV_MANY_0"), "the first and the last are gone");

    /* the removal fails: the environment is NOT clean and the answer says so (-1), with the name that is left and never a value */
    HvEnvOps failing = {unset_fail_one}, lying = {unset_lie_one}, allfail = {unset_fail_all};
    setenv("HV_A", "1", 1); setenv("HV_FAIL", "SECRETVAL", 1); setenv("HV_B", "1", 1);
    n = hv_runner_scrub_env_ops(&failing, names, sizeof names, left, sizeof left);
    CHECK(n == -1 && !strcmp(left, "HV_FAIL") && !strstr(left, "SECRETVAL") && !strchr(left, '='), "unsetenv fails for one name: -1, left = '%s'", left);
    CHECK(strstr(names, "HV_A") && strstr(names, "HV_B") && !getenv("HV_A") && !getenv("HV_B") && getenv("HV_FAIL"), "the others were still removed (%s), the one that failed is still there", names);
    clear_hv();
    /* unsetenv says yes and the variable is still there: caught by looking again */
    setenv("HV_LIE", "SECRETVAL", 1); setenv("HV_C", "1", 1);
    n = hv_runner_scrub_env_ops(&lying, names, sizeof names, left, sizeof left);
    CHECK(n == -1 && !strcmp(left, "HV_LIE") && !strstr(names, "HV_LIE"), "unsetenv lies for one name: -1, left = '%s', not counted as removed ('%s')", left, names);
    clear_hv();
    /* unsetenv reports an error although the variable is gone: still not trusted (an error from the call is an error) */
    HvEnvOps rcbad = {unset_rc_but_removed};
    setenv("HV_Z", "1", 1);
    n = hv_runner_scrub_env_ops(&rcbad, names, sizeof names, left, sizeof left);
    CHECK(n == -1 && !strcmp(left, "HV_Z") && names[0] == 0, "unsetenv reports an error but the variable is gone: -1 all the same ('%s')", left);
    clear_hv();
    /* the failing one in the middle of many, and a second failing one after it */
    for (int i = 0; i < 50; i++) { char nm[32]; snprintf(nm, sizeof nm, "HV_M_%d", i); setenv(nm, "1", 1); if (i == 20) setenv("HV_FAIL", "1", 1); }
    n = hv_runner_scrub_env_ops(&failing, names, 4096 > sizeof names ? sizeof names : 4096, left, sizeof left);
    int still = 0; for (int i = 0; i < 50; i++) { char nm[32]; snprintf(nm, sizeof nm, "HV_M_%d", i); still += getenv(nm) != NULL; }
    CHECK(n == -1 && still == 0 && !strcmp(left, "HV_FAIL"), "50 + one failing: all 50 removed, only HV_FAIL left ('%s')", left);
    clear_hv();
    setenv("HV_X", "1", 1); setenv("HV_Y", "1", 1);
    n = hv_runner_scrub_env_ops(&allfail, names, sizeof names, left, sizeof left);
    CHECK(n == -1 && strstr(left, "HV_X") && strstr(left, "HV_Y") && names[0] == 0, "every removal fails: -1, both left, none counted as removed");
    clear_hv();
    /* a name too long to handle is not skipped silently */
    char longname[300]; memset(longname, 'Q', sizeof longname); memcpy(longname, "HV_", 3); longname[299] = 0;
    setenv(longname, "1", 1);
    n = hv_runner_scrub_env(names, sizeof names, left, sizeof left);
    CHECK(n == -1 && left[0] != 0, "a name too long to handle: -1 ('%.30s')", left);
    unsetenv(longname);
    CHECK(hv_runner_scrub_env(names, sizeof names, left, sizeof left) == 0, "clean again afterwards");
    /* small output buffers do not overflow or change the answer */
    setenv("HV_LONGER_NAME_ONE", "1", 1); setenv("HV_LONGER_NAME_TWO", "1", 1);
    char tiny[8];
    n = hv_runner_scrub_env(tiny, sizeof tiny, tiny, 0);
    CHECK(n == 2 && strlen(tiny) < sizeof tiny, "tiny buffers: still 2 removed, no overflow");
}

/* ---- the disk layer's refusal (what readonly_storage = 1 turns on) ---- */
static void test_readonly_disk(void) {
    char p[256]; snprintf(p, sizeof p, "%s/ro.img", g_dir);
    int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0600);
    uint8_t blk[512]; memset(blk, 0xA5, sizeof blk);
    CHECK(fd >= 0 && write(fd, blk, sizeof blk) == sizeof blk && ftruncate(fd, 65536) == 0, "test image made");
    close(fd);
    HvDisk d; uint8_t w[512], back[512]; uint64_t calls = 0, refused = 0;
    memset(w, 0x11, sizeof w);
    CHECK(hv_disk_open(&d, p, 0, 1) == 0, "open read-only");
    CHECK(hv_disk_write(&d, 0, w, sizeof w) == -1, "a write is refused");
    hv_disk_write_stats(&d, &calls, &refused);
    CHECK(calls == 1 && refused == 1, "counted: asked %llu, refused %llu", (unsigned long long)calls, (unsigned long long)refused);
    CHECK(hv_disk_read(&d, 0, back, sizeof back) == 0 && !memcmp(back, blk, sizeof blk), "the data is unchanged");
    hv_disk_close(&d);
    fd = open(p, O_RDONLY); CHECK(fd >= 0 && pread(fd, back, sizeof back, 0) == sizeof back && !memcmp(back, blk, sizeof blk), "and so is the file"); close(fd);
}

int main(void) {
    const char *t = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/c1vm_XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(g_dir)) { perror("mkdtemp"); return 1; }
    test_config();
    test_env();
    test_commands();
    test_picture();
    test_pointer();
    test_threads();
    test_readonly_disk();
    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) printf("(could not remove %s)\n", g_dir);
    printf("c1vm: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
