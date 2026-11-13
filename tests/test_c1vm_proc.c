/* C1-2: host/ui_runner_vm.c as a real separate process with the REAL virtual machine in it (host/vm.c), driven through its standard input and the shared file.
   Linux arm64 only (the engine runs arm64 guest code natively), no network, no real firmware: the ROM is a stand-in (`b .` repeated) and the disks are blank files made here.
   Usage: test_c1vm_proc <ui_runner_vm binary>
   What a pass shows: the runner starts with the safety settings it prints, the machine is made and runs, input commands in the shared ring become calls into the real hv_vm_*
   input functions without a crash (counted by the runner), a missing boot picture and a missing pointer reach the shared file as "not there", every way of stopping ends in a
   confirmed stop and exit code 0, options that would loosen the settings are refused, HV_* environment variables do not turn on an installer boot, and the two disk files are
   byte-for-byte and time-stamp unchanged afterwards.
   What it does NOT show: that a guest saw the input (no guest software runs: `b .` never reads a device), a real boot picture or pointer, anything about macOS. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "frame_shm.h"

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void msleep(int ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, NULL); }

static const char *g_bin;
static char g_dir[160];
static char g_rom[200], g_disk[200], g_aux[200], g_shm[200];

/* ---- files ---- */
static int make_sparse(const char *p, off_t n) { int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0600); if (fd < 0) return -1; int r = ftruncate(fd, n); close(fd); return r; }
static int make_aux(const char *p) {
    static const unsigned char hd[24] = {0xa5, 0xd5, 0xb3, 0xfe, 0x97, 0xad, 0x1d, 0x27, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0600); if (fd < 0) return -1;
    int ok = write(fd, hd, sizeof hd) == (ssize_t)sizeof hd && ftruncate(fd, 33570816) == 0; close(fd); return ok ? 0 : -1;
}
static int make_rom(const char *p) {
    int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0600); if (fd < 0) return -1;
    uint32_t b_dot = 0x14000000u; int ok = 1;
    for (int i = 0; i < 16384 && ok; i++) ok = write(fd, &b_dot, 4) == 4;
    close(fd); return ok ? 0 : -1;
}
typedef struct { uint64_t hash; off_t size; time_t mtime; long mtime_ns; } FileId;
static int file_id(const char *p, FileId *o) {
    int fd = open(p, O_RDONLY); if (fd < 0) return -1;
    struct stat st; fstat(fd, &st);
    uint64_t h = 1469598103934665603ull; static unsigned char buf[1 << 20]; ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) for (ssize_t i = 0; i < n; i++) h = (h ^ buf[i]) * 1099511628211ull;
    close(fd);
    o->hash = h; o->size = st.st_size; o->mtime = st.st_mtim.tv_sec; o->mtime_ns = st.st_mtim.tv_nsec;
    return 0;
}
static int same_id(const FileId *a, const FileId *b) { return a->hash == b->hash && a->size == b->size && a->mtime == b->mtime && a->mtime_ns == b->mtime_ns; }

/* ---- process ---- */
typedef struct { pid_t pid; int in, out; char buf[16384]; size_t n; } Proc;
static Proc spawn(const char *const *argv) {
    Proc r; memset(&r, 0, sizeof r); r.pid = -1; r.in = r.out = -1;
    int pin[2], pout[2];
    if (pipe(pin) || pipe(pout)) return r;
    pid_t pid = fork();
    if (pid == 0) { dup2(pin[0], 0); dup2(pout[1], 1); dup2(pout[1], 2); close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]); execv(argv[0], (char *const *)argv); _exit(127); }
    close(pin[0]); close(pout[1]);
    r.pid = pid; r.in = pin[1]; r.out = pout[0];
    fcntl(r.out, F_SETFL, O_NONBLOCK);
    return r;
}
static void drain(Proc *p) { ssize_t k; while (p->n + 1 < sizeof p->buf && (k = read(p->out, p->buf + p->n, sizeof p->buf - 1 - p->n)) > 0) p->n += (size_t)k; p->buf[p->n] = 0; }
static int wait_for(Proc *p, const char *needle, double sec) { double t = now(); while (now() - t < sec) { drain(p); if (strstr(p->buf, needle)) return 1; msleep(10); } drain(p); return strstr(p->buf, needle) != NULL; }
static int finish(Proc *p, double sec, double *took) {      /* exit code, or -1 when it did not end in time (it is then killed) */
    int st = 0, code = -1; double t = now();
    while (now() - t < sec) { pid_t r = waitpid(p->pid, &st, WNOHANG); if (r == p->pid) { code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st); break; } drain(p); msleep(5); }
    if (took) *took = now() - t;
    if (code == -1) { kill(p->pid, SIGKILL); waitpid(p->pid, &st, 0); }
    drain(p);
    if (p->in >= 0) close(p->in);
    close(p->out); p->in = p->out = -1;
    return code;
}
static void say(Proc *p, const char *s) { if (write(p->in, s, strlen(s)) < 0) { /* the runner may already be gone */ } }
static HvShm *open_running(pid_t pid, double sec) {
    HvShm *s = NULL; double t = now();
    while (now() - t < sec) { if (hv_shm_open(g_shm, &s) == 0) { HvShmStats st; if (hv_shm_stats(s, &st) == 0 && st.state == HV_SHM_RUNNING && st.runner_pid == (uint32_t)pid) return s; hv_shm_close(s); s = NULL; } msleep(20); }
    return NULL;
}

typedef struct { unsigned long long popped, disp, skipped, rejected, calls, queries, dpres, dabs, pvalid, pabs, cerr, serr, frames, rpush, rpop, rdrop, wasked, wrefused, nettx, netrx; } Line;
static int parse_line(const char *text, const char *tag, Line *o) {
    char key[40]; snprintf(key, sizeof key, "ui_runner_vm %s:", tag);
    const char *p = strstr(text, key); if (!p) return 0;
    const char *last = p; while ((p = strstr(p + 1, key))) last = p;
    int n = sscanf(last, "ui_runner_vm %*[a-z]: pid %*d cmds popped %llu dispatched %llu skipped %llu rejected %llu | calls %llu queries %llu | display present %llu absent %llu | pointer valid %llu absent %llu | call_errors %llu shm_errors %llu | frames published %llu | ring pushed %llu popped %llu dropped %llu | disk writes asked %llu refused %llu | net frames tx %llu rx %llu",
                   &o->popped, &o->disp, &o->skipped, &o->rejected, &o->calls, &o->queries, &o->dpres, &o->dabs, &o->pvalid, &o->pabs, &o->cerr, &o->serr, &o->frames, &o->rpush, &o->rpop, &o->rdrop, &o->wasked, &o->wrefused, &o->nettx, &o->netrx);
    return n == 20;
}

static Proc start_runner(const char *const *extra) {
    const char *argv[32]; int n = 0;
    argv[n++] = g_bin; argv[n++] = "--shm"; argv[n++] = g_shm; argv[n++] = "--rom"; argv[n++] = g_rom; argv[n++] = "--disk"; argv[n++] = g_disk; argv[n++] = "--aux"; argv[n++] = g_aux;
    argv[n++] = "--ram-mb"; argv[n++] = "256"; argv[n++] = "--jit-mb"; argv[n++] = "64"; argv[n++] = "--cpus"; argv[n++] = "2";
    for (int i = 0; extra && extra[i]; i++) argv[n++] = extra[i];
    argv[n] = NULL;
    return spawn(argv);
}
static void fresh_files(void) { unlink(g_shm); make_sparse(g_disk, 16 << 20); make_aux(g_aux); make_rom(g_rom); }

/* ---- scenarios ---- */
static void test_life_and_input(void) {
    printf("-- life and input\n");
    Proc p = start_runner(NULL);
    CHECK(p.pid > 0, "spawned");
    HvShm *app = open_running(p.pid, 30);
    CHECK(app != NULL, "the runner made the shared file and reached RUNNING");
    if (!app) { double t; int c = finish(&p, 5, &t); printf("   output:\n%s\n   exit %d\n", p.buf, c); return; }
    CHECK(wait_for(&p, "started pid", 5) && wait_for(&p, "config (the structure passed to hv_vm_create):", 5), "it announced itself and the structure it passes to hv_vm_create");
    CHECK(strstr(p.buf, "readonly_storage=1 no_net=1 no_sound=1 installer=0 identity=none host_jit=none") != NULL, "the printed structure: read-only, no network, no installer, no identity, no host JIT memory");
    CHECK(strstr(p.buf, "usb device port present: 0 (must be 0)") != NULL, "the machine has no USB device port");
    CHECK(strstr(p.buf, "env: 0 HV_* variable(s) removed") != NULL, "no HV_* variables were set (none removed)");
    /* 11 valid commands of every kind and 2 invalid ones */
    int pushed = 0, bad_pushed = 0;
    for (int k = 0; k < 3; k++) { pushed += hv_shm_push_cmd(app, HV_SHM_CMD_KEY, 30 + k, 1) == 1; pushed += hv_shm_push_cmd(app, HV_SHM_CMD_KEY, 30 + k, 0) == 1; }
    for (int k = 0; k < 2; k++) pushed += hv_shm_push_cmd(app, HV_SHM_CMD_MOVE, 4 - k, -4) == 1;
    pushed += hv_shm_push_cmd(app, HV_SHM_CMD_BUTTON, 0, 1) == 1;
    pushed += hv_shm_push_cmd(app, HV_SHM_CMD_WHEEL, 1, 0) == 1;
    pushed += hv_shm_push_cmd(app, HV_SHM_CMD_HWHEEL, -1, 0) == 1;
    bad_pushed += hv_shm_push_cmd(app, HV_SHM_CMD_KEY, 0, 1) == 1;
    bad_pushed += hv_shm_push_cmd(app, 77, 0, 0) == 1;
    CHECK(pushed == 11 && bad_pushed == 2, "13 commands queued (%d + %d)", pushed, bad_pushed);
    HvShmStats st; double t0 = now();
    while (now() - t0 < 10) { if (hv_shm_stats(app, &st) == 0 && st.cmds_popped >= 13) break; msleep(20); }
    CHECK(st.cmds_popped == 13 && st.cmds_pushed == 13, "the runner took all 13 out of the ring (popped %llu)", (unsigned long long)st.cmds_popped);
    int px = -9, py = -9;
    CHECK(hv_shm_get_pointer(app, &px, &py) == 0, "no guest pointer reaches the file (nothing to report one)");
    uint64_t seen = 0; uint32_t w, h; static uint8_t pix[64];
    CHECK(hv_shm_read_frame(app, &seen, pix, sizeof pix, &w, &h) == -1, "no boot picture in the file");
    uint64_t hb0 = st.heartbeat; msleep(300); hv_shm_stats(app, &st);
    CHECK(st.heartbeat > hb0, "the heartbeat advances (%llu -> %llu)", (unsigned long long)hb0, (unsigned long long)st.heartbeat);
    say(&p, "status\n");
    CHECK(wait_for(&p, "ui_runner_vm status:", 5), "a status line on request");
    Line l; memset(&l, 0, sizeof l);
    CHECK(parse_line(p.buf, "status", &l), "the status line parses");
    CHECK(l.popped == 13 && l.disp == 11 && l.rejected == 2 && l.skipped == 0, "popped %llu = dispatched %llu + rejected %llu + skipped %llu", l.popped, l.disp, l.rejected, l.skipped);
    CHECK(l.calls == 11, "calls made into the machine: %llu (the 11 valid commands)", l.calls);
    CHECK(l.dpres == 0 && l.dabs > 0 && l.pvalid == 0 && l.pabs > 0, "the machine said 'no picture' %llu times and 'no pointer' %llu times, never 'present'", l.dabs, l.pabs);
    CHECK(l.cerr == 0 && l.serr == 0 && l.frames == 0, "no contract errors (%llu), no shared-file errors (%llu), no picture published (%llu)", l.cerr, l.serr, l.frames);
    say(&p, "quit\n");
    double took; int code = finish(&p, 12, &took);
    CHECK(code == 0, "exit code 0 after 'quit' (got %d)", code);
    CHECK(strstr(p.buf, "RESULT: stopped, confirmed (quit)") != NULL, "RESULT says stopped, confirmed");
    printf("   (stop took %.2f s)\n", took);
    memset(&l, 0, sizeof l);
    CHECK(parse_line(p.buf, "final", &l), "a final status line");
    CHECK(l.popped == 13 && l.wasked == 0 && l.wrefused == 0, "final: disk writes asked %llu, refused %llu", l.wasked, l.wrefused);
    CHECK(l.nettx == 0 && l.netrx == 0, "final: no network frames (tx %llu rx %llu)", l.nettx, l.netrx);
    hv_shm_stats(app, &st);
    CHECK(st.state == HV_SHM_STOPPED, "the shared file says STOPPED");
    hv_shm_close(app);
}

static void test_stops(void) {
    printf("-- every way of stopping\n");
    struct { const char *name; const char *extra[4]; } way[] = {{"input closed", {NULL}}, {"signal", {NULL}}, {"app closed", {NULL}}, {"time limit", {"--seconds", "2", NULL}}};
    for (int i = 0; i < 4; i++) {
        fresh_files();
        Proc p = start_runner(way[i].extra[0] ? way[i].extra : NULL);
        HvShm *app = open_running(p.pid, 30);
        CHECK(app != NULL, "[%s] running", way[i].name);
        if (!app) { finish(&p, 3, NULL); continue; }
        msleep(500);
        if (i == 0) { close(p.in); p.in = -1; }
        else if (i == 1) kill(p.pid, SIGTERM);
        else if (i == 2) hv_shm_app_close(app);
        double took; int code = finish(&p, 12, &took);
        char want[64]; snprintf(want, sizeof want, "stopped, confirmed (%s)", way[i].name);
        CHECK(code == 0 && strstr(p.buf, want), "[%s] exit 0 and '%s' (exit %d, %.2f s)", way[i].name, want, code, took);
        HvShmStats st; CHECK(hv_shm_stats(app, &st) == 0 && st.state == HV_SHM_STOPPED, "[%s] state STOPPED", way[i].name);
        hv_shm_close(app);
    }
}

static void test_refusals(void) {
    printf("-- options and settings that would loosen the rules\n");
    const char *loosening[][4] = {{"--installer", NULL}, {"--net", NULL}, {"--ecid", "5", NULL}, {"--seed", "5", NULL}, {"--readonly", "0", NULL}, {"--write", NULL}, {"--no-readonly", NULL}, {"--ipsw", "x.ipsw", NULL}};
    for (size_t i = 0; i < sizeof loosening / sizeof loosening[0]; i++) {
        fresh_files();
        Proc p = start_runner(loosening[i]);
        double t; int code = finish(&p, 10, &t);
        CHECK(code == 2 && strstr(p.buf, "unknown or incomplete argument") != NULL, "'%s' is refused: exit 2 (got %d)", loosening[i][0], code);
        CHECK(access(g_shm, F_OK) != 0, "'%s': no shared file was made", loosening[i][0]);
    }
    /* a required argument missing */
    { const char *argv[] = {g_bin, "--shm", g_shm, "--rom", g_rom, "--disk", g_disk, NULL}; fresh_files(); Proc p = spawn(argv); double t; int code = finish(&p, 10, &t); CHECK(code == 2, "no --aux: exit 2 (got %d)", code); }
    /* sizes outside the ranges the settings check allows: refused before anything is made */
    const char *bad[][4] = {{"--ram-mb", "8", NULL}, {"--cpus", "99", NULL}, {"--jit-mb", "1", NULL}, {"--jit-mb", "4096", NULL}};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        fresh_files();
        Proc p = start_runner(bad[i]);
        double t; int code = finish(&p, 10, &t);
        CHECK(code == 1 && strstr(p.buf, "the settings were refused") != NULL && access(g_shm, F_OK) != 0, "%s %s: settings refused, exit 1, no shared file (got %d)", bad[i][0], bad[i][1], code);
    }
    /* a disk that does not exist: the machine cannot be made; the file says STOPPED */
    { fresh_files(); unlink(g_disk); Proc p = start_runner(NULL); double t; int code = finish(&p, 15, &t);
      CHECK(code == 1 && strstr(p.buf, "the machine could not be made") != NULL, "a missing disk: exit 1, 'could not be made' (got %d)", code);
      HvShm *s = NULL; HvShmStats st;
      CHECK(hv_shm_open(g_shm, &s) == 0 && hv_shm_stats(s, &st) == 0 && st.state == HV_SHM_STOPPED, "and the shared file says STOPPED");
      if (s) hv_shm_close(s);
      CHECK(access(g_disk, F_OK) != 0, "the runner did not create the missing disk"); }
}

static void test_env(void) {
    printf("-- HV_* variables do not switch anything on\n");
    fresh_files();
    char ramfile[240]; snprintf(ramfile, sizeof ramfile, "%s/ramfile", g_dir);
    setenv("HV_RUN_INSTALLER", "1,1", 1); setenv("HV_USB_ON_NORMAL", "1", 1); setenv("HV_VUSB_PROBE", "1", 1); setenv("HV_RAM_FILE", ramfile, 1);
    Proc p = start_runner(NULL);
    unsetenv("HV_RUN_INSTALLER"); unsetenv("HV_USB_ON_NORMAL"); unsetenv("HV_VUSB_PROBE"); unsetenv("HV_RAM_FILE");
    HvShm *app = open_running(p.pid, 30);
    CHECK(app != NULL, "running with the four variables set in its environment");
    CHECK(wait_for(&p, "env: 4 HV_* variable(s) removed", 5), "it removed exactly the four");
    CHECK(strstr(p.buf, "HV_RUN_INSTALLER") && strstr(p.buf, "HV_USB_ON_NORMAL") && strstr(p.buf, "HV_VUSB_PROBE") && strstr(p.buf, "HV_RAM_FILE") && !strstr(p.buf, "1,1"), "it names them (not their values)");
    CHECK(strstr(p.buf, "usb device port present: 0 (must be 0)") != NULL, "no USB device port although HV_USB_ON_NORMAL was set (the engine would have made one)");
    msleep(500);
    CHECK(access(ramfile, F_OK) != 0, "no RAM file was made although HV_RAM_FILE was set");
    say(&p, "quit\n");
    double t; int code = finish(&p, 12, &t);
    CHECK(code == 0, "and it stops normally (exit %d)", code);
    if (app) hv_shm_close(app);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: test_c1vm_proc <ui_runner_vm binary>\n"); return 2; }
    g_bin = argv[1];
    const char *t = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/c1vmp_XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(g_dir)) { perror("mkdtemp"); return 1; }
    snprintf(g_rom, sizeof g_rom, "%s/rom.bin", g_dir); snprintf(g_disk, sizeof g_disk, "%s/blank_disk.img", g_dir);
    snprintf(g_aux, sizeof g_aux, "%s/blank_aux.img", g_dir); snprintf(g_shm, sizeof g_shm, "%s/ui.shm", g_dir);
    signal(SIGPIPE, SIG_IGN);
    fresh_files();
    test_life_and_input();
    test_stops();
    test_refusals();
    test_env();
    /* one more start on untouched files, so that the time stamp comparison is meaningful: files made, the runner runs and stops, the files are compared */
    printf("-- the disk files are untouched\n");
    FileId id_disk0, id_aux0;
    fresh_files(); file_id(g_disk, &id_disk0); file_id(g_aux, &id_aux0);
    msleep(1100);   /* a write would give a different time stamp */
    { Proc p = start_runner(NULL); HvShm *app = open_running(p.pid, 30); msleep(1000); say(&p, "quit\n"); double tk; int code = finish(&p, 12, &tk); if (app) hv_shm_close(app); CHECK(code == 0, "the run ended normally"); }
    FileId d2, a2; file_id(g_disk, &d2); file_id(g_aux, &a2);
    CHECK(same_id(&id_disk0, &d2), "the disk image: same bytes, size and time stamp");
    CHECK(same_id(&id_aux0, &a2), "the auxiliary storage: same bytes, size and time stamp");
    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) printf("(could not remove %s)\n", g_dir);
    printf("c1vm_proc: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
