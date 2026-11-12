/* C1-1: the runner process, SYNTHETIC mode. It is the process that will own the virtual machine (C1-2); here there is no machine at all: a simulated guest takes the
   commands and a synthetic picture is published. It exists to test what C1 needs before a machine is attached: the process boundary, the shared file
   (host/frame_shm.h), input going in, the pointer coming back, and a stop that is confirmed or reported as not confirmed.

   What it will NOT do, by construction (tools/check-runner-safe.sh checks the binary for it):
     - it links no virtual machine, no restore / TSS / USB / install code, no curl or TLS, and it imports no socket, connect or name-resolution function: it cannot reach a network
     - it opens exactly one file, the shared file named by --shm (created here, mode 0600), and reads standard input
     - it takes no ROM, disk or aux image, and has no option that would
   Arguments: --shm PATH (required)   --stop-wait-ms N (how long the stop waits for the simulated machine, default 5000)   --seconds N (stop by itself after N seconds, default never)
   Standard input: "quit" or end of input = stop; "status" = print one status line. Nothing else is accepted (an unknown line is ignored and counted).
   Exit codes: 0 stopped and confirmed; 2 bad arguments or the shared file could not be made; 3 the stop was NOT confirmed within the wait (the machine thread was still running:
   the process ends anyway, and the line says so).
   Output (standard output, line by line): "ui_runner: ..." and one "ui_runner RESULT: ..." at the end. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "display_buf.h"
#include "frame_shm.h"

#define W 1280
#define H 720

static HvShm *g_shm;
static atomic_int g_stop, g_exited, g_sig;
static struct { int x, y; long moves, down, up, wheel, hwheel, keys, last_key, bad; } g_guest;   /* written by the machine thread, read by status_line: under g_gm */
static pthread_mutex_t g_gm = PTHREAD_MUTEX_INITIALIZER;
static atomic_ullong g_frames_made;

static int accel(int d) { double a = 1.0 + (abs(d) / 40.0 > 1.5 ? 1.5 : abs(d) / 40.0); return (int)(d * a + (d < 0 ? -0.5 : 0.5)); }

static void apply(const HvShmCmd *c) {
    switch (c->type) {
    case HV_SHM_CMD_KEY: g_guest.keys++; g_guest.last_key = c->a; break;
    case HV_SHM_CMD_MOVE:
        g_guest.moves++;
        g_guest.x += accel(c->a); g_guest.y += accel(c->b);
        g_guest.x = g_guest.x < 0 ? 0 : g_guest.x > W - 1 ? W - 1 : g_guest.x;
        g_guest.y = g_guest.y < 0 ? 0 : g_guest.y > H - 1 ? H - 1 : g_guest.y;
        break;
    case HV_SHM_CMD_BUTTON: if (c->b) g_guest.down++; else g_guest.up++; break;
    case HV_SHM_CMD_WHEEL: g_guest.wheel += c->a; break;
    case HV_SHM_CMD_HWHEEL: g_guest.hwheel += c->a; break;
    default: g_guest.bad++; break;
    }
}

static void *machine(void *arg) {
    (void)arg;
    uint8_t *px = malloc((size_t)W * H * 4);
    if (!px) { atomic_store(&g_exited, 1); return NULL; }
    uint32_t frame = 0;
    struct timespec nap = {0, 16 * 1000 * 1000};
    while (!atomic_load(&g_stop)) {
        HvShmCmd cmds[64];
        int n;
        while ((n = hv_shm_pop_cmds(g_shm, cmds, 64)) > 0) { pthread_mutex_lock(&g_gm); for (int i = 0; i < n; i++) apply(&cmds[i]); pthread_mutex_unlock(&g_gm); }
        pthread_mutex_lock(&g_gm); int gx = g_guest.x, gy = g_guest.y; pthread_mutex_unlock(&g_gm);
        hv_testpattern(px, W, H, W * 4, ++frame, gx, gy);
        hv_shm_publish(g_shm, px, W, H, W * 4);
        hv_shm_set_pointer(g_shm, 1, gx, gy);
        hv_shm_heartbeat(g_shm);
        atomic_store(&g_frames_made, frame);
        nanosleep(&nap, NULL);
    }
    free(px);
    atomic_store(&g_exited, 1);
    return NULL;
}

static void on_signal(int s) { atomic_store(&g_sig, s); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void status_line(const char *tag) {
    HvShmStats st;
    int ok = hv_shm_stats(g_shm, &st) == 0;
    pthread_mutex_lock(&g_gm);
    typeof(g_guest) gg = g_guest;
    pthread_mutex_unlock(&g_gm);
    printf("ui_runner %s: pid %d frames %llu published %llu refused %llu cmds applied %llu dropped %llu moves %ld buttons %ld/%ld wheel %ld hwheel %ld keys %ld (last %ld) pointer %d,%d recovered %u%s\n",
           tag, (int)getpid(), (unsigned long long)atomic_load(&g_frames_made), ok ? (unsigned long long)st.frames : 0ull, ok ? (unsigned long long)st.rejected : 0ull,
           ok ? (unsigned long long)st.cmds_applied : 0ull, ok ? (unsigned long long)st.cmds_dropped : 0ull, gg.moves, gg.down, gg.up, gg.wheel, gg.hwheel,
           gg.keys, gg.last_key, gg.x, gg.y, ok ? st.recovered : 0u, ok ? "" : " (stats lock not taken)");
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *path = NULL;
    long stop_wait_ms = 5000, seconds = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shm") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--stop-wait-ms") && i + 1 < argc) stop_wait_ms = atol(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atol(argv[++i]);
        else { fprintf(stderr, "ui_runner: unknown or incomplete argument '%s' (accepted: --shm PATH, --stop-wait-ms N, --seconds N)\n", argv[i]); return 2; }
    }
    if (!path || stop_wait_ms < 0 || stop_wait_ms > 600000 || seconds < 0) { fprintf(stderr, "ui_runner: --shm PATH is required; the numbers must be sensible\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    if (hv_shm_create(path, &g_shm) != 0) { fprintf(stderr, "ui_runner: cannot make the shared file: %s\n", strerror(errno)); return 2; }
    g_guest.x = W / 2; g_guest.y = H / 2;
    printf("ui_runner: started pid %d, synthetic mode (no machine), picture %dx%d\n", (int)getpid(), W, H);
    fflush(stdout);

    pthread_t th;
    if (pthread_create(&th, NULL, machine, NULL) != 0) { printf("ui_runner RESULT: FAILED to start the simulated machine\n"); hv_shm_set_state(g_shm, HV_SHM_STOPPED); return 2; }
    hv_shm_set_state(g_shm, HV_SHM_RUNNING);

    const char *why = "quit";
    double t0 = now();
    char line[256]; size_t ll = 0; long ignored = 0;
    for (;;) {
        if (atomic_load(&g_sig)) { why = "signal"; break; }
        if (hv_shm_app_closed(g_shm)) { why = "app closed"; break; }
        if (seconds && now() - t0 >= (double)seconds) { why = "time limit"; break; }
        struct pollfd p = {0, POLLIN, 0};
        int r = poll(&p, 1, 50);
        if (r < 0 && errno != EINTR) { why = "input error"; break; }
        if (r > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
            char c;
            ssize_t n = read(0, &c, 1);
            if (n == 0) { why = "input closed"; break; }
            if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; why = "input error"; break; }
            if (c == '\n') {
                line[ll] = 0; ll = 0;
                if (!strcmp(line, "quit")) { why = "quit"; break; }
                else if (!strcmp(line, "status")) status_line("status");
                else ignored++;
            } else if (ll + 1 < sizeof line) line[ll++] = c;
        }
    }

    /* The stop, in order: no more input or pictures (the flag), the machine thread's confirmation, the shared file's last state. */
    hv_shm_set_state(g_shm, HV_SHM_STOPPING);
    atomic_store(&g_stop, 1);
    double ts = now();
    while (!atomic_load(&g_exited) && (now() - ts) * 1000 < (double)stop_wait_ms) usleep(2000);
    int confirmed = atomic_load(&g_exited);
    if (confirmed) pthread_join(th, NULL);
    hv_shm_set_state(g_shm, HV_SHM_STOPPED);
    status_line("final");
    printf("ui_runner RESULT: %s (%s), ignored input lines %ld\n", confirmed ? "stopped, confirmed" : "STOP NOT CONFIRMED within the wait; ending the process anyway", why, ignored);
    fflush(stdout);
    if (confirmed) hv_shm_close(g_shm);   /* with the thread still running the mapping is left for the process end */
    return confirmed ? 0 : 3;
}
