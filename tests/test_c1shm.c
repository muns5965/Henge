/* C1-1 tests (Linux, no VM, no Android, no network): host/frame_shm.c, and host/ui_runner.c as a real separate process.
   Build and run: tools/c1-test.sh <build dir> (it builds both; the runner's path is argv[1]). Exit 0 = every check passed. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../host/display_buf.h"
#include "../host/frame_shm.h"
#include "../host/touch_input.h"

static int g_fail, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void group(const char *n) { printf("-- %s\n", n); fflush(stdout); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void msleep(int ms) { usleep((useconds_t)ms * 1000); }

/* ThreadSanitizer makes everything several times slower: the time limits are stretched */
#if defined(__SANITIZE_THREAD__)
static const double K = 4;
#else
static const double K = 1;
#endif
static char g_dir[256];
static const char *runner_path;
static char *shm_path(const char *name) { static char b[8][400]; static int k; char *p = b[k++ & 7]; snprintf(p, 400, "%s/%s", g_dir, name); return p; }

/* ---------------- basics ---------------- */
static void test_basic(void) {
    group("shm: create, open, publish, read, rejects, commands, pointer");
    HvShm *a = NULL, *b = NULL;
    char *p = shm_path("basic.shm");
    CHECK(hv_shm_open(p, &b) != 0 && !b, "opening a file that is not there fails");
    CHECK(hv_shm_create(p, &a) == 0 && a, "create");
    CHECK(hv_shm_open(p, &b) == 0 && b, "open");
    struct stat st; CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0600, "mode 0600 (%o)", (unsigned)(st.st_mode & 0777));
    HvShmStats s; CHECK(hv_shm_stats(b, &s) == 0 && s.state == HV_SHM_STARTING && s.runner_pid == (uint32_t)getpid() && s.frames == 0, "initial state");
    uint64_t seen = 0; uint32_t w = 0, h = 0; static uint8_t buf[HV_SHM_PIXEL_BYTES];
    CHECK(hv_shm_read_frame(b, &seen, buf, sizeof buf, &w, &h) == -1, "no picture yet");
    /* a picture with padding in its rows */
    uint32_t pw = 130, ph = 70, stride = pw * 4 + 24;
    uint8_t *src = malloc((size_t)stride * ph);
    for (uint32_t y = 0; y < ph; y++) for (uint32_t x = 0; x < stride; x++) src[(size_t)y * stride + x] = (uint8_t)(x < pw * 4 ? (x * 7 + y * 13) : 0xEE);
    CHECK(hv_shm_publish(a, src, pw, ph, stride) == 1, "publish");
    CHECK(hv_shm_read_frame(b, &seen, buf, sizeof buf, &w, &h) == 1 && w == pw && h == ph && seen == 1, "read it (seq %llu)", (unsigned long long)seen);
    int same = 1; for (uint32_t y = 0; y < ph && same; y++) same = memcmp(buf + (size_t)y * pw * 4, src + (size_t)y * stride, (size_t)pw * 4) == 0;
    CHECK(same, "pixels equal, padding dropped (tightly packed)");
    CHECK(hv_shm_read_frame(b, &seen, buf, sizeof buf, &w, &h) == 0, "nothing newer");
    CHECK(hv_shm_read_frame(b, &(uint64_t){0}, buf, 100, &w, &h) == -1, "destination too small");
    CHECK(hv_shm_publish(a, src, pw, ph, stride) == 1 && hv_shm_read_frame(b, &seen, buf, sizeof buf, &w, &h) == 1 && seen == 2, "next picture is newer");
    /* rejects: the old picture stays */
    CHECK(hv_shm_publish(a, src, 0, ph, stride) == 0, "zero width");
    CHECK(hv_shm_publish(a, src, pw, 0, stride) == 0, "zero height");
    CHECK(hv_shm_publish(a, src, pw, ph, pw * 4 - 1) == 0, "row shorter than the pixels");
    CHECK(hv_shm_publish(a, NULL, pw, ph, stride) == 0, "no pixels");
    CHECK(hv_shm_publish(a, src, 4096, 4096, 4096 * 4) == 0, "over the size limit");
    CHECK(hv_shm_publish(a, src, 0xffffffffu, 0xffffffffu, 0xffffffffu) == 0, "overflowing size");
    CHECK(hv_shm_read_frame(b, &seen, buf, sizeof buf, &w, &h) == 0, "the rejects changed nothing");
    hv_shm_stats(b, &s); CHECK(s.frames == 2 && s.rejected == 6 && s.width == pw && s.height == ph, "counts: frames %llu rejected %llu", (unsigned long long)s.frames, (unsigned long long)s.rejected);
    free(src);
    /* commands */
    HvShmCmd c[8];
    CHECK(hv_shm_pop_cmds(a, c, 8) == 0, "no commands");
    CHECK(hv_shm_push_cmd(b, HV_SHM_CMD_MOVE, 5, -6) == 1 && hv_shm_push_cmd(b, HV_SHM_CMD_BUTTON, 0, 1) == 1 && hv_shm_push_cmd(b, HV_SHM_CMD_KEY, 30, 1) == 1, "push three");
    int n = hv_shm_pop_cmds(a, c, 2);
    CHECK(n == 2 && c[0].type == HV_SHM_CMD_MOVE && c[0].a == 5 && c[0].b == -6 && c[1].type == HV_SHM_CMD_BUTTON && c[1].b == 1, "first two, in order");
    n = hv_shm_pop_cmds(a, c, 8);
    CHECK(n == 1 && c[0].type == HV_SHM_CMD_KEY && c[0].a == 30, "the third");
    /* ring: full, then wraparound over many more than the slots */
    int queued = 0, dropped = 0;
    for (unsigned i = 0; i < HV_SHM_CMD_SLOTS + 100; i++) { int r = hv_shm_push_cmd(b, HV_SHM_CMD_WHEEL, (int)i, 0); if (r == 1) queued++; else if (r == 0) dropped++; }
    CHECK(queued == (int)HV_SHM_CMD_SLOTS && dropped == 100, "full ring: %d queued, %d dropped", queued, dropped);
    int expect = 0, bad = 0, got = 0;
    while ((n = hv_shm_pop_cmds(a, c, 8)) > 0) for (int i = 0; i < n; i++, got++) if (c[i].a != expect++) bad++;
    CHECK(got == (int)HV_SHM_CMD_SLOTS && !bad, "all %d come out in order (%d wrong)", got, bad);
    int wrong = 0;
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < 300; i++) hv_shm_push_cmd(b, HV_SHM_CMD_HWHEEL, round * 1000 + i, 0);
        int e = 0; while ((n = hv_shm_pop_cmds(a, c, 8)) > 0) for (int i = 0; i < n; i++) if (c[i].a != round * 1000 + e++) wrong++;
        if (e != 300) wrong++;
    }
    CHECK(wrong == 0, "wraparound keeps order over 1500 commands");
    hv_shm_stats(b, &s); CHECK(s.cmds_dropped == 100 && s.cmds_pushed == s.cmds_applied, "stats: dropped %llu pushed %llu applied %llu", (unsigned long long)s.cmds_dropped, (unsigned long long)s.cmds_pushed, (unsigned long long)s.cmds_applied);
    /* pointer and state */
    int px = -1, py = -1;
    CHECK(hv_shm_get_pointer(b, &px, &py) == 0 && px == -1, "no pointer until the runner says so (outputs untouched)");
    hv_shm_set_pointer(a, 1, 640, 360);
    CHECK(hv_shm_get_pointer(b, &px, &py) == 1 && px == 640 && py == 360, "pointer");
    hv_shm_set_pointer(a, 0, 1, 1);
    CHECK(hv_shm_get_pointer(b, &px, &py) == 0, "pointer withdrawn");
    hv_shm_set_state(a, HV_SHM_RUNNING); hv_shm_heartbeat(a); hv_shm_heartbeat(a);
    hv_shm_stats(b, &s); CHECK(s.state == HV_SHM_RUNNING && s.heartbeat == 2, "state and heartbeat visible to the other mapping");
    CHECK(!hv_shm_app_closed(a), "not closed"); hv_shm_app_close(b); CHECK(hv_shm_app_closed(a), "the runner sees the app's close");
    /* a new create replaces the file; the old mapping stays valid */
    HvShm *a2 = NULL; CHECK(hv_shm_create(p, &a2) == 0, "create again");
    hv_shm_publish(a2, (const uint8_t[16]){0}, 2, 2, 8);
    HvShm *b2 = NULL; CHECK(hv_shm_open(p, &b2) == 0, "open the new one");
    hv_shm_stats(b2, &s); CHECK(s.frames == 1, "the new file is a fresh one");
    hv_shm_stats(b, &s); CHECK(s.frames == 2, "the old mapping still reads its own file");
    hv_shm_close(a2); hv_shm_close(b2); hv_shm_close(a); hv_shm_close(b);
    /* bad files */
    char *q = shm_path("short.shm"); int fd = open(q, O_CREAT | O_RDWR, 0600); CHECK(fd >= 0 && ftruncate(fd, 100) == 0, "make a short file"); close(fd);
    CHECK(hv_shm_open(q, &b) != 0, "a file of the wrong size is refused");
    q = shm_path("zero.shm"); fd = open(q, O_CREAT | O_RDWR, 0600); CHECK(fd >= 0 && ftruncate(fd, 16384 + HV_SHM_PIXEL_BYTES) == 0, "make a right-sized, uninitialised file"); close(fd);
    CHECK(hv_shm_open(q, &b) != 0, "an uninitialised file (no magic) is refused");
    q = shm_path("link.shm"); CHECK(symlink(shm_path("basic.shm"), q) == 0, "make a symlink");
    CHECK(hv_shm_open(q, &b) != 0, "a symlink is not followed on open");
    CHECK(hv_shm_create(q, &a) == 0, "create over a symlink replaces the link itself");
    struct stat ls; CHECK(lstat(q, &ls) == 0 && S_ISREG(ls.st_mode), "and what is there is now a regular file"); hv_shm_close(a);
}

/* ---------------- threads (TSan): no torn pictures, commands in order ---------------- */
static HvShm *tA, *tB; static atomic_int tstop;
static void *producer(void *x) { (void)x; static uint8_t px[256 * 128 * 4]; for (unsigned f = 1; f <= 1500; f++) { memset(px, (int)(f & 0xff), sizeof px); hv_shm_publish(tA, px, 256, 128, 256 * 4); if (f % 50 == 0) msleep(1); } atomic_store(&tstop, 1); return NULL; }
static atomic_long torn, readn;
static void *consumer(void *x) { (void)x; static uint8_t px[256 * 128 * 4]; uint64_t seen = 0; uint32_t w, h;
    while (!atomic_load(&tstop)) { if (hv_shm_read_frame(tB, &seen, px, sizeof px, &w, &h) == 1) { atomic_fetch_add(&readn, 1); for (size_t i = 1; i < sizeof px; i++) if (px[i] != px[0]) { atomic_fetch_add(&torn, 1); break; } } }
    return NULL; }
static atomic_long order_bad, popped;
static void *cmd_producer(void *x) { (void)x; for (int i = 0; i < 20000;) { int r = hv_shm_push_cmd(tB, HV_SHM_CMD_MOVE, i, 0); if (r == 1) i++; else usleep(50); } atomic_store(&tstop, 1); return NULL; }
static void *cmd_consumer(void *x) { (void)x; int expect = 0; HvShmCmd c[32];
    for (;;) { int n = hv_shm_pop_cmds(tA, c, 32); for (int i = 0; i < n; i++) { if (c[i].a != expect++) atomic_fetch_add(&order_bad, 1); } atomic_fetch_add(&popped, n); if (n == 0) { if (atomic_load(&tstop)) { n = hv_shm_pop_cmds(tA, c, 32); for (int i = 0; i < n; i++) { if (c[i].a != expect++) atomic_fetch_add(&order_bad, 1); } atomic_fetch_add(&popped, n); if (n == 0) break; } else usleep(50); } }
    return NULL; }
static void test_threads(void) {
    group("shm: threads (pictures are never torn, commands keep order)");
    char *p = shm_path("thr.shm");
    CHECK(hv_shm_create(p, &tA) == 0 && hv_shm_open(p, &tB) == 0, "create and open (two mappings)");
    pthread_t a, b; atomic_store(&tstop, 0);
    pthread_create(&a, NULL, producer, NULL); pthread_create(&b, NULL, consumer, NULL); pthread_join(a, NULL); pthread_join(b, NULL);
    CHECK(atomic_load(&torn) == 0 && atomic_load(&readn) > 0, "read %ld pictures, %ld torn", atomic_load(&readn), atomic_load(&torn));
    atomic_store(&tstop, 0);
    pthread_create(&a, NULL, cmd_producer, NULL); pthread_create(&b, NULL, cmd_consumer, NULL); pthread_join(a, NULL); pthread_join(b, NULL);
    CHECK(atomic_load(&popped) == 20000 && atomic_load(&order_bad) == 0, "20000 commands: popped %ld, out of order %ld", atomic_load(&popped), atomic_load(&order_bad));
    hv_shm_close(tA); hv_shm_close(tB);
}

/* ---------------- processes: a locker killed at random moments ---------------- */
#if defined(__SANITIZE_THREAD__)
/* ThreadSanitizer does not know robust mutexes: after pthread_mutex_consistent it still believes the dead owner holds the lock and reports the next unlock. The recovery is
   covered by the plain and the AddressSanitizer runs. */
static void test_dead_owner(void) { group("shm: dead-owner recovery (skipped under ThreadSanitizer, which does not model robust mutexes)"); }
#else
static void test_dead_owner(void) {
    group("shm: a process killed while it publishes (the lock is recovered, the picture is never half written)");
    HvShm *a = NULL; char *p = shm_path("dead.shm");
    CHECK(hv_shm_create(p, &a) == 0, "create");
    static uint8_t buf[HV_SHM_PIXEL_BYTES];
    int stuck = 0, torn_seen = 0, frames_seen = 0, killed = 0;
    for (int i = 0; i < 80; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            HvShm *c = NULL; if (hv_shm_open(p, &c) != 0) _exit(2);
            uint32_t w = 1920, h = 1080; uint8_t *px = malloc((size_t)w * h * 4);
            for (unsigned f = 1;; f++) { memset(px, (int)(f & 0xff), (size_t)w * h * 4); hv_shm_publish(c, px, w, h, w * 4); }
        }
        msleep(8 + (i * 7) % 23);          /* different moments in the copy loop */
        kill(pid, SIGKILL); int st; waitpid(pid, &st, 0); killed++;
        uint64_t seen = 0; uint32_t w, h; HvShmStats s;
        if (hv_shm_stats(a, &s) != 0) stuck++;
        int r = hv_shm_read_frame(a, &seen, buf, sizeof buf, &w, &h);
        if (r == -1 && s.width == 0) { /* picture dropped by a recovery, or none yet: fine */ }
        else if (r == 1) { frames_seen++; for (size_t k = 1; k < (size_t)w * h * 4; k++) if (buf[k] != buf[0]) { torn_seen++; break; } }
        else if (r != 0) stuck++;
    }
    HvShmStats s; hv_shm_stats(a, &s);
    CHECK(stuck == 0, "the parent was never stuck (%d)", stuck);
    CHECK(torn_seen == 0, "no half-written picture was ever read (%d of %d)", torn_seen, frames_seen);
    CHECK(s.recovered >= 1, "at least one kill landed inside the lock and was recovered (%u of %d)", s.recovered, killed);
    printf("   (recovered %u, pictures read %d, killed %d)\n", s.recovered, frames_seen, killed);
    CHECK(hv_shm_publish(a, (const uint8_t[16]){1}, 2, 2, 8) == 1, "publishing works afterwards");
    uint64_t seen = 0; uint32_t w, h; CHECK(hv_shm_read_frame(a, &seen, buf, sizeof buf, &w, &h) == 1 && w == 2, "and so does reading");
    hv_shm_close(a);
}

#endif

/* ---------------- the runner as a real process ---------------- */
typedef struct { pid_t pid; int in, out; } Proc;
static pid_t g_last_pid;
static Proc spawn(const char *const *argv) {
    int pin[2], pout[2]; Proc r = {-1, -1, -1};
    if (pipe(pin) || pipe(pout)) return r;
    pid_t pid = fork();
    if (pid == 0) { dup2(pin[0], 0); dup2(pout[1], 1); dup2(pout[1], 2); close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]); execv(argv[0], (char *const *)argv); _exit(127); }
    close(pin[0]); close(pout[1]);
    r.pid = pid; r.in = pin[1]; r.out = pout[0]; g_last_pid = pid;
    return r;
}
static int finish(Proc *p, double wait_s, char *out, size_t cap) {      /* exit code, or -1 if it did not end in time (it is then killed) */
    int st = 0, code = -1; double t = now();
    while (now() - t < wait_s) { pid_t r = waitpid(p->pid, &st, WNOHANG); if (r == p->pid) { code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st); break; } msleep(5); }
    if (code == -1) { kill(p->pid, SIGKILL); waitpid(p->pid, &st, 0); }
    size_t n = 0; if (out && cap) { ssize_t k; fcntl(p->out, F_SETFL, O_NONBLOCK); while (n + 1 < cap && (k = read(p->out, out + n, cap - 1 - n)) > 0) n += (size_t)k; out[n] = 0; }
    if (p->in >= 0) close(p->in);
    close(p->out); p->in = p->out = -1;
    return code;
}
/* the runner `pid` has made its file and is running (a file left by an earlier runner at the same path is not it) */
static HvShm *wait_open_pid(const char *path, double sec, pid_t pid) {
    HvShm *s = NULL; double t = now();
    while (now() - t < sec * K) { if (hv_shm_open(path, &s) == 0) { HvShmStats st; if (hv_shm_stats(s, &st) == 0 && st.state == HV_SHM_RUNNING && (pid <= 0 || st.runner_pid == (uint32_t)pid)) return s; hv_shm_close(s); s = NULL; } msleep(10); }
    return NULL;
}
#define wait_open(path, sec) wait_open_pid(path, sec, g_last_pid)
static int frame_number(HvShm *s, uint64_t *seen) {          /* the number the test pattern carries, or -1 */
    static uint8_t bgra[1280 * 720 * 4]; static uint8_t rgbx[1280 * 720 * 4]; uint32_t w, h;
    if (hv_shm_read_frame(s, seen, bgra, sizeof bgra, &w, &h) != 1 || w != 1280 || h != 720) return -1;
    hv_disp_bgra_to_rgbx(bgra, w, h, w * 4, rgbx, w, w, h);
    return hv_testpattern_frame_rgbx(rgbx, w, w, h);
}

static void test_runner_life(void) {
    group("runner: a real process (start, pictures, input, pointer, stop paths)");
    char *p = shm_path("run1.shm"); char out[4096];
    const char *argv[] = {runner_path, "--shm", p, NULL};
    Proc r = spawn(argv);
    HvShm *s = wait_open(p, 5);
    CHECK(s != NULL, "the runner started and the file says RUNNING");
    if (!s) { finish(&r, 1, out, sizeof out); return; }
    HvShmStats st; hv_shm_stats(s, &st);
    CHECK(st.runner_pid == (uint32_t)r.pid, "the file names the runner's pid");
    uint64_t seen = 0; int f1 = -1; double t = now();
    while (f1 < 0 && now() - t < 3) { f1 = frame_number(s, &seen); msleep(10); }
    CHECK(f1 >= 0, "a picture arrived (frame %d)", f1);
    msleep(200); int f2 = -1; t = now(); while (f2 < 0 && now() - t < 3) { f2 = frame_number(s, &seen); msleep(10); }
    CHECK(f2 > f1, "and a later one (frame %d)", f2);
    uint64_t hb = st.heartbeat; msleep(100); hv_shm_stats(s, &st); CHECK(st.heartbeat > hb, "the heartbeat grows");
    /* input in, pointer back */
    int x = -1, y = -1; t = now(); while (hv_shm_get_pointer(s, &x, &y) != 1 && now() - t < 2) msleep(10);
    CHECK(x == 640 && y == 360, "the guest pointer starts in the middle (%d,%d)", x, y);
    hv_shm_push_cmd(s, HV_SHM_CMD_MOVE, 10, 0); hv_shm_push_cmd(s, HV_SHM_CMD_MOVE, -100, 40);
    for (int i = 0; i < 3; i++) hv_shm_push_cmd(s, HV_SHM_CMD_BUTTON, 0, 1), hv_shm_push_cmd(s, HV_SHM_CMD_BUTTON, 0, 0);
    hv_shm_push_cmd(s, HV_SHM_CMD_WHEEL, 3, 0); hv_shm_push_cmd(s, HV_SHM_CMD_HWHEEL, -2, 0); hv_shm_push_cmd(s, HV_SHM_CMD_KEY, 30, 1); hv_shm_push_cmd(s, HV_SHM_CMD_KEY, 30, 0);
    msleep(150);
    t = now(); int px = 0, py = 0; while (now() - t < 2) { hv_shm_get_pointer(s, &px, &py); if (px != 640) break; msleep(10); }
    CHECK(px == 640 + 10 * 1 + (int)lround(-100 * (1 + 1.5)) + 0 - 0 + (int)lround(10 * (1 + 0.25)) - 10 && py == 360 + (int)lround(40 * 2.0), "pointer moved by the runner's accelerated guest to %d,%d", px, py);
    write(r.in, "status\n", 7); msleep(200);
    write(r.in, "bogus line\n", 11);
    write(r.in, "quit\n", 5);
    int code = finish(&r, 5, out, sizeof out);
    CHECK(code == 0, "quit: exit code %d", code);
    CHECK(strstr(out, "ui_runner RESULT: stopped, confirmed (quit)") != NULL, "the result line says stopped, confirmed");
    CHECK(strstr(out, "buttons 3/3") && strstr(out, "wheel 3 hwheel -2") && strstr(out, "keys 2 (last 30)") && strstr(out, "ignored input lines 1"), "the commands arrived exactly: %s", strstr(out, "ui_runner status") ? strstr(out, "ui_runner status") : "(no status line)");
    hv_shm_stats(s, &st); CHECK(st.state == HV_SHM_STOPPED, "the file says STOPPED (%d)", st.state);
    hv_shm_close(s);
}

static void test_runner_stops(void) {
    group("runner: every way of being stopped ends the same way, and arguments it must refuse");
    char out[4096]; int code;
    struct { const char *name; int how; const char *why; } cases[] = {{"eof", 0, "input closed"}, {"sigterm", 1, "signal"}, {"appclose", 2, "app closed"}, {"timelimit", 3, "time limit"}};
    for (int i = 0; i < 4; i++) {
        char *p = shm_path(cases[i].name);
        const char *argv[] = {runner_path, "--shm", p, "--seconds", cases[i].how == 3 ? "1" : "0", NULL};
        Proc r = spawn(argv); HvShm *s = wait_open(p, 5);
        CHECK(s != NULL, "%s: started", cases[i].name); if (!s) { finish(&r, 1, out, sizeof out); continue; }
        msleep(100);
        if (cases[i].how == 0) { close(r.in); r.in = -1; }
        else if (cases[i].how == 1) kill(r.pid, SIGTERM);
        else if (cases[i].how == 2) hv_shm_app_close(s);
        double t0 = now(); code = finish(&r, 5, out, sizeof out);
        CHECK(code == 0 && now() - t0 < 4, "%s: exit %d in %.2f s", cases[i].name, code, now() - t0);
        char want[128]; snprintf(want, sizeof want, "(%s)", cases[i].why);
        CHECK(strstr(out, "stopped, confirmed") && strstr(out, want), "%s: says why (%s)", cases[i].name, cases[i].why);
        HvShmStats st; hv_shm_stats(s, &st); CHECK(st.state == HV_SHM_STOPPED, "%s: file STOPPED", cases[i].name); hv_shm_close(s);
    }
    /* refused arguments: it has no option for a ROM, a disk, a network or anything else */
    const char *bad[][8] = {{runner_path, NULL}, {runner_path, "--rom", "x", "--shm", "y", NULL}, {runner_path, "--shm", NULL}, {runner_path, "--shm", "z", "--disk", "d", NULL},
                            {runner_path, "--shm", "z", "--net", NULL}, {runner_path, "--shm", "z", "--installer", NULL}, {runner_path, "--shm", "z", "--stop-wait-ms", "-5", NULL}};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        Proc r = spawn(bad[i]); code = finish(&r, 3, out, sizeof out);
        CHECK(code == 2 && strstr(out, "ui_runner:") != NULL, "bad arguments #%u: exit %d, and it says why", i, code);
    }
    /* the shared file cannot be made: a folder that is not there */
    const char *argv[] = {runner_path, "--shm", "/nonexistent-dir-c1/x.shm", NULL};
    Proc r = spawn(argv); code = finish(&r, 3, out, sizeof out); CHECK(code == 2, "no folder: exit %d", code);
}

static void test_runner_killed(void) {
    group("runner: killed (SIGKILL), and a new one in its place");
    char out[2048]; char *p = shm_path("kill.shm");
    const char *argv[] = {runner_path, "--shm", p, NULL};
    Proc r = spawn(argv); HvShm *s = wait_open(p, 5);
    CHECK(s != NULL, "started"); if (!s) { finish(&r, 1, out, sizeof out); return; }
    msleep(150); HvShmStats a, b; hv_shm_stats(s, &a);
    kill(r.pid, SIGKILL); int code = finish(&r, 3, out, sizeof out); CHECK(code == 128 + SIGKILL, "killed (%d)", code);
    msleep(150); hv_shm_stats(s, &b);
    CHECK(b.heartbeat == a.heartbeat || b.heartbeat - a.heartbeat < 20, "after the kill the heartbeat stops (%llu -> %llu)", (unsigned long long)a.heartbeat, (unsigned long long)b.heartbeat);
    uint64_t h1 = b.heartbeat; msleep(200); hv_shm_stats(s, &b); CHECK(b.heartbeat == h1, "and stays stopped");
    CHECK(b.state == HV_SHM_RUNNING, "the file still says RUNNING: the heartbeat, not the state, is how a dead runner is told");
    CHECK(hv_shm_push_cmd(s, HV_SHM_CMD_MOVE, 1, 1) >= 0 && hv_shm_get_pointer(s, &(int){0}, &(int){0}) >= 0, "the app can still use its mapping (nothing hangs)");
    Proc r2 = spawn(argv); HvShm *s2 = wait_open(p, 5);
    CHECK(s2 != NULL, "a new runner on the same path starts");
    if (s2) { HvShmStats c; hv_shm_stats(s2, &c); CHECK(c.runner_pid == (uint32_t)r2.pid && c.cmds_pushed == 0, "it is a fresh file (new pid, empty ring)"); hv_shm_close(s2); }
    write(r2.in, "quit\n", 5); code = finish(&r2, 5, out, sizeof out); CHECK(code == 0, "and it stops cleanly (%d)", code);
    hv_shm_close(s);
}

/* the C0 touch loop with the guest on the far side of a process boundary */
static HvShm *loop_shm;
static void l_move(int dx, int dy) { hv_shm_push_cmd(loop_shm, HV_SHM_CMD_MOVE, dx, dy); }
static void l_btn(int b, int d) { hv_shm_push_cmd(loop_shm, HV_SHM_CMD_BUTTON, b, d); }
static void l_key(uint16_t c, int d) { hv_shm_push_cmd(loop_shm, HV_SHM_CMD_KEY, c, d); }
static void l_whl(int n) { hv_shm_push_cmd(loop_shm, HV_SHM_CMD_WHEEL, n, 0); }
static void l_hwl(int n) { hv_shm_push_cmd(loop_shm, HV_SHM_CMD_HWHEEL, n, 0); }
static void test_runner_touch_loop(void) {
    group("runner: the touch loop closed across the process boundary");
    char out[4096]; char *p = shm_path("loop.shm");
    const char *argv[] = {runner_path, "--shm", p, NULL};
    Proc r = spawn(argv); HvShm *s = wait_open(p, 5);
    CHECK(s != NULL, "started"); if (!s) { finish(&r, 1, out, sizeof out); return; }
    loop_shm = s;
    HvInputSink sk = {l_key, l_move, l_btn, l_whl, l_hwl};
    HvTouch t; hv_touch_init(&t, &sk); hv_touch_set_display(&t, 1280, 720); hv_touch_set_view(&t, 0, 0, 1440, 2555);
    struct { float x, y; } pts[] = {{720, 1277}, {5, 5}, {1435, 2550}, {300, 2000}, {1271, 480}, {684, 1545}, {100, 100}};
    double t0 = now(); int ok = 0; int64_t ms = 1000;
#if defined(__SANITIZE_THREAD__)
    /* Under ThreadSanitizer the runner takes ~100-200 ms per picture, far more than the touch loop's 50 ms wait for feedback, so the loop (damped 0.6 against a guest that
       accelerates up to 2.5x) is fed stale positions and does not converge. That is a property of the loop, not of the file; this run only exercises the traffic for races. */
    const unsigned npts = 2;
#else
    const unsigned npts = sizeof pts / sizeof pts[0];
#endif
    for (unsigned i = 0; i < npts; i++) {
        hv_touch_event(&t, HV_TOUCH_DOWN, 0, pts[i].x, pts[i].y, ms);
        double tt = now(); int arrived = 0, x = 0, y = 0;
        while (now() - tt < 3) {
            if (hv_shm_get_pointer(s, &x, &y) == 1) hv_touch_pointer(&t, 1, x, y); else hv_touch_pointer(&t, 0, 0, 0);
            ms += 10; hv_touch_tick(&t, ms);
            HvTouchDiag d; hv_touch_diag(&t, &d);
            if (d.has_target && fabs(d.target_x - x) <= 1.5 && fabs(d.target_y - y) <= 1.5) { arrived = 1; break; }
            msleep(10);
        }
        ms += 50; hv_touch_event(&t, HV_TOUCH_UP, 0, pts[i].x, pts[i].y, ms);
#if !defined(__SANITIZE_THREAD__)
        CHECK(arrived, "touch %u (%.0f,%.0f): the pointer arrived (%d,%d) in %.2f s", i, pts[i].x, pts[i].y, x, y, now() - tt);
#endif
        ok += arrived; msleep(30);
        for (int k = 0; k < 5; k++) { ms += 10; hv_touch_tick(&t, ms); msleep(10); }
    }
    printf("   (%d of %u arrived, %.1f s)\n", ok, npts, now() - t0);
    write(r.in, "quit\n", 5); int code = finish(&r, 5, out, sizeof out); CHECK(code == 0, "stopped (%d)", code);
    CHECK(strstr(out, "buttons ") != NULL, "it counted the clicks too: %s", strstr(out, "ui_runner final") ? strstr(out, "ui_runner final") : "?");
    hv_shm_close(s);
}

#if !defined(__SANITIZE_THREAD__)
static void test_app_dies(void) {
    group("runner: the app dies while pushing commands (the runner goes on and stops cleanly)");
    char out[2048]; char *p = shm_path("appdie.shm");
    const char *argv[] = {runner_path, "--shm", p, NULL};
    Proc r = spawn(argv); HvShm *s = wait_open(p, 5);
    CHECK(s != NULL, "started"); if (!s) { finish(&r, 1, out, sizeof out); return; }
    hv_shm_close(s);
    for (int i = 0; i < 20; i++) {
        pid_t pid = fork();
        if (pid == 0) { HvShm *c = NULL; if (hv_shm_open(p, &c) != 0) _exit(2); static uint8_t px[1280 * 720 * 4]; uint64_t seen = 0; uint32_t w, h; for (;;) { hv_shm_push_cmd(c, HV_SHM_CMD_MOVE, 1, 1); hv_shm_read_frame(c, &seen, px, sizeof px, &w, &h); seen = 0; } }
        msleep(10 + (i * 5) % 17); kill(pid, SIGKILL); int st; waitpid(pid, &st, 0);
    }
    s = NULL; CHECK(hv_shm_open(p, &s) == 0, "reopen after the deaths");
    HvShmStats a, b; hv_shm_stats(s, &a); msleep(150); hv_shm_stats(s, &b);
    CHECK(b.heartbeat > a.heartbeat, "the runner kept running (heartbeat %llu -> %llu)", (unsigned long long)a.heartbeat, (unsigned long long)b.heartbeat);
    write(r.in, "quit\n", 5); int code = finish(&r, 5, out, sizeof out);
    CHECK(code == 0, "and stopped cleanly (%d)", code);
    printf("   (locks recovered: %u)\n", b.recovered);
    hv_shm_close(s);
}
#endif

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: test_c1shm RUNNER\n"); return 2; }
    runner_path = argv[1];
    snprintf(g_dir, sizeof g_dir, "/tmp/c1shm-test-%d", (int)getpid());
    mkdir(g_dir, 0700);
    signal(SIGPIPE, SIG_IGN);
    test_basic();
    test_dead_owner();        /* forks: before any thread exists */
    test_runner_life(); test_runner_stops(); test_runner_killed(); test_runner_touch_loop();
#if !defined(__SANITIZE_THREAD__)
    test_app_dies();          /* the runner recovers a dead owner's lock: ThreadSanitizer does not model robust mutexes (see test_dead_owner) */
#endif
    test_threads();
    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir); if (system(cmd)) {}
    printf("c1shm: %d check(s), %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
