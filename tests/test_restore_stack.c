/* The whole restore stack (libirecovery, usbmuxd, libimobiledevice, libtatsu, idevicerestore, the HTTP stand-in, host/install.c) started the way the
   app will start it, but with an IPSW that does not exist and a USB port with nothing behind it. It shows that the stack loads and its threads start on
   this platform, that usbmuxd listens, and that a restore which cannot start ends with a clean error and a progress callback instead of a hang or a crash.
   It does NOT talk to a guest, a signing server or an IPSW.
   usage: test_restore_stack [ipsw-path [workdir]] */
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../devices/pcie.h"
#include "../devices/virtio_usb.h"
#include "../host/install.h"

/* virtio_usb.c's links to the guest-memory side of the machine: never reached here */
int hv_virtq_pop(HvVirtio *v, int q, HvVirtqElem *e) { (void)v, (void)q, (void)e; return 0; }
void hv_virtq_push(HvVirtio *v, int q, const HvVirtqElem *e, uint32_t w) { (void)v, (void)q, (void)e, (void)w; }
void hv_virtio_interrupt(HvVirtio *v) { (void)v; }
void hv_virtio_init(HvVirtio *v, HvMachine *m, int id, uint32_t cc, int nq, uint64_t f) { (void)v, (void)m, (void)id, (void)cc, (void)nq, (void)f; }
uint32_t hv_virtq_read(HvVirtio *v, const HvVirtqElem *e, uint32_t o, void *b, uint32_t l) { (void)v, (void)e, (void)o, (void)b, (void)l; return 0; }
uint32_t hv_virtq_write(HvVirtio *v, const HvVirtqElem *e, uint32_t o, const void *b, uint32_t l) { (void)v, (void)e, (void)o, (void)b, (void)l; return 0; }
uint32_t hv_virtq_writable(const HvVirtqElem *e) { (void)e; return 0; }
int hv_pcie_attach(HvPcie *p, HvPciDev *d) { (void)p, (void)d; return -1; }
void hv_machine_wake(HvMachine *m) { (void)m; }

static volatile int last_step = -1, steps;
static void on_progress(int step, double p, void *user) { (void)p, (void)user; last_step = step; steps++; printf("progress callback: step %d\n", step); fflush(stdout); }

static int mux_listening(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    int ok = connect(s, (struct sockaddr *)&a, sizeof a) == 0;
    close(s);
    return ok;
}

int main(int argc, char **argv) {
    const char *ipsw = argc > 1 ? argv[1] : "/nonexistent/no-such.ipsw";
    const char *work = argc > 2 ? argv[2] : "/tmp";
    setvbuf(stdout, NULL, _IONBF, 0);
    HvVirtioUsb *u = calloc(1, sizeof *u);
    pthread_mutex_init(&u->mu, NULL);
    pthread_cond_init(&u->cv, NULL);
    int rc = hv_install_start(u, ipsw, work, 0x1122334455ull, on_progress, NULL);
    printf("hv_install_start -> %d\n", rc);
    if (rc != 0) return 2;
    int port = 27100 + (int)(getpid() % 800);
    int listening = 0;
    for (int i = 0; i < 40 && !listening; i++) { listening = mux_listening(port); if (!listening) usleep(100000); }
    printf("MUX_LISTENING=%d (127.0.0.1:%d)\n", listening, port);
    time_t t0 = time(NULL);
    while (hv_install_result() == 1 && time(NULL) - t0 < 40) usleep(100000);
    printf("RESTORE_RESULT=%d AFTER_S=%ld LAST_STEP=%d PROGRESS_CALLS=%d\n", hv_install_result(), (long)(time(NULL) - t0), last_step, steps);
    fflush(stdout);
    _exit(hv_install_result() < 0 && listening ? 0 : 1); /* the mux thread never returns on its own: leave without waiting for it */
}
