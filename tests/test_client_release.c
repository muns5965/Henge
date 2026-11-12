/* Step 3: per-client async handler tracker and idevicerestore_client_release().
   r_race   : handlers finish at random times while another thread releases the client (ASan/TSan: no access after free)
   r_block  : a handler is blocked past the deadline -> release returns -1, nothing freed, client still valid, new registration refused; after it ends -> 0
   r_closed : registration after closing is refused
   r_null   : NULL client -> 0 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "idevicerestore.h"
#include "common.h"

static volatile int g_hold;
struct h { struct idevicerestore_client_t *c; struct hv_async_tracker *trk; int us; int hold; volatile int *tag; };
static void *handler(void *a) {
    struct h *q = a; struct idevicerestore_client_t *c = q->c; struct hv_async_tracker *trk = q->trk; int us = q->us, hold = q->hold; free(q);
    if (hold) while (__atomic_load_n(&g_hold, __ATOMIC_SEQ_CST)) usleep(1000);
    usleep((unsigned)us);
    __atomic_store_n(&c->async_err, 0, __ATOMIC_SEQ_CST);   /* last access to the client */
    hv_async_task_done(trk);                                 /* only the tracker after this */
    return NULL;
}
static int spawn(struct idevicerestore_client_t *c, int us, int hold) {
    if (hv_async_task_register(c->async_trk) != 0) return -1;
    struct h *q = malloc(sizeof *q); q->c = c; q->trk = c->async_trk; q->us = us; q->hold = hold;
    pthread_t t; pthread_create(&t, NULL, handler, q); pthread_detach(t); return 0;
}
static void *rel_thread(void *a) { struct idevicerestore_client_t *c = a; return (void *)(intptr_t)idevicerestore_client_release_timeout(c, 5000); }

int main(int argc, char **argv) {
    const char *k = argc > 1 ? argv[1] : "";
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!strcmp(k, "r_null")) { printf("CR_RESULT r_null: release(NULL)=%d\n", idevicerestore_client_release(NULL)); return 0; }
    if (!strcmp(k, "r_race")) {
        int bad = 0; srand(7);
        for (int r = 0; r < 300; r++) {
            struct idevicerestore_client_t *c = idevicerestore_client_new();
            int n = 1 + rand() % 6, reg = 0;
            for (int i = 0; i < n; i++) if (spawn(c, rand() % 2000, 0) == 0) reg++;
            pthread_t t; pthread_create(&t, NULL, rel_thread, c);
            usleep((unsigned)(rand() % 1500));
            void *rv; pthread_join(t, &rv);
            if ((intptr_t)rv != 0) bad++;
        }
        printf("CR_RESULT r_race: 300 rounds, release failures %d\n", bad);
        printf("CR_VERDICT r_race: %s\n", bad == 0 ? "OK" : "FAILED");
        usleep(200000); return 0;
    }
    if (!strcmp(k, "r_block")) {
        struct idevicerestore_client_t *c = idevicerestore_client_new();
        __atomic_store_n(&g_hold, 1, __ATOMIC_SEQ_CST);
        spawn(c, 0, 1);
        int r1 = idevicerestore_client_release_timeout(c, 300);
        long keep = c->ecid; (void)keep;                                   /* the client must still be valid (ASan) */
        int r2 = spawn(c, 0, 0);                                           /* closing: refused */
        printf("CR_RESULT r_block: release with a blocked handler rc=%d (expect -1), registration after that rc=%d (expect -1)\n", r1, r2);
        __atomic_store_n(&g_hold, 0, __ATOMIC_SEQ_CST);
        int r3 = idevicerestore_client_release_timeout(c, 5000);
        printf("CR_RESULT r_block: release after the handler ended rc=%d (expect 0)\n", r3);
        printf("CR_VERDICT r_block: %s\n", (r1 == -1 && r2 == -1 && r3 == 0) ? "OK" : "FAILED");
        usleep(100000); return 0;
    }
    if (!strcmp(k, "r_closed")) {
        struct idevicerestore_client_t *c = idevicerestore_client_new();
        hv_async_tracker_close(c->async_trk);
        int r = spawn(c, 0, 0);
        int rr = idevicerestore_client_release(c);
        printf("CR_RESULT r_closed: register after close=%d (expect -1), release=%d (expect 0)\n", r, rr);
        printf("CR_VERDICT r_closed: %s\n", (r == -1 && rr == 0) ? "OK" : "FAILED");
        return 0;
    }
    printf("unknown case\n"); return 2;
}
