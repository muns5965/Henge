/* devices/usb_diag_gate.h: the first events are printed, then every 2000th with one summary line; thread-safe. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../devices/usb_diag_gate.h"

static int fails;
#define CHECK(c, ...) do { int ok_ = (c); printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
static uint64_t g_ev; static volatile int g_pass_count;

static void *worker(void *a) { (void)a; int p = 0; for (int i = 0; i < 25000; i++) p += hv_usb_diag_pass(&g_ev); __atomic_add_fetch(&g_pass_count, p, __ATOMIC_SEQ_CST); return NULL; }

int main(void) {
    char path[] = "/tmp/udg_XXXXXX"; int fd = mkstemp(path); if (fd < 0) return 2;
    fflush(stderr); int save = dup(2); dup2(fd, 2);
    uint64_t e = 0; int passes = 0, first_100 = 0;
    for (int i = 1; i <= 100000; i++) { int p = hv_usb_diag_pass(&e); passes += p; if (i <= 100) first_100 += p; }
    pthread_t t[4]; for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, NULL); for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    fflush(stderr); dup2(save, 2); close(save);
    char buf[1 << 16]; lseek(fd, 0, SEEK_SET); ssize_t n = read(fd, buf, sizeof buf - 1); if (n < 0) n = 0; buf[n] = 0; close(fd); unlink(path);
    int lines = 0; for (char *q = buf; (q = strstr(q, "events so far")) != NULL; q++) lines++;
    CHECK(first_100 == 100, "the first %u events are all printed", HV_USB_DIAG_KEEP);
    CHECK(passes == 150, "100000 events: the first 100 + every 2000th after them (50) = 150 printed (%d)", passes);
    CHECK(lines == 100, "one summary line per sampled event after the first 100: 50 single-threaded + 50 from the 4 threads (%d lines)", lines);
    CHECK(g_pass_count == 150, "4 threads (100000 calls at once): exactly as many printed as single-threaded, 150 (%d)", g_pass_count);
    printf(fails ? "USBDIAGGATE FAILED %d\n" : "USBDIAGGATE OK (0 failed)\n", fails);
    return fails ? 1 : 0;
}
