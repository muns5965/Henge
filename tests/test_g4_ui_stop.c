/* Isolated stop-request state tests. No VM, no restore stack, no network. */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include "g4_ui_stop.h"

static HvG4UiStop stop_state;
static atomic_int session_active, attempts, accepted;
static void fake_cancel(void) {
    atomic_fetch_add(&attempts, 1);
    if (atomic_load(&session_active)) atomic_store(&accepted, 1);
}
static void reset(void) {
    atomic_store(&stop_state.requested, 0);
    atomic_store(&session_active, 0);
    atomic_store(&attempts, 0);
    atomic_store(&accepted, 0);
}
static atomic_int worker_started;
static void *racing_worker(void *arg) {
    (void)arg;
    if (!hv_g4_ui_start_allowed(&stop_state)) return NULL;
    atomic_store(&worker_started, 1);
    sched_yield(); /* permit a stop request before creation of a session */
    atomic_store(&session_active, 1);
    sched_yield(); /* permit a stop request immediately after session creation */
    hv_g4_ui_after_start(&stop_state, fake_cancel);
    return NULL;
}
static void test_threaded_race(void) {
    for (int i = 0; i < 1000; ++i) {
        reset();
        atomic_store(&worker_started, 0);
        pthread_t thread;
        assert(pthread_create(&thread, NULL, racing_worker, NULL) == 0);
        if (i % 3 == 1) sched_yield();
        hv_g4_ui_request_stop(&stop_state, fake_cancel);
        assert(pthread_join(thread, NULL) == 0);
        /* A skipped start needs no cancel. Every started session must be
           cancelled, regardless of how the two threads were scheduled. */
        if (atomic_load(&worker_started)) assert(atomic_load(&accepted));
        else assert(!atomic_load(&session_active));
    }
    puts("PASS 1000 actual pthread start/stop interleavings");
}
int main(void) {
    reset();
    hv_g4_ui_request_stop(&stop_state, fake_cancel);
    assert(!hv_g4_ui_start_allowed(&stop_state));
    assert(atomic_load(&attempts) == 1 && !atomic_load(&accepted));
    puts("PASS stop before worker starts");

    reset();
    assert(hv_g4_ui_start_allowed(&stop_state));
    hv_g4_ui_request_stop(&stop_state, fake_cancel);
    assert(!atomic_load(&accepted));
    atomic_store(&session_active, 1);
    hv_g4_ui_after_start(&stop_state, fake_cancel);
    assert(atomic_load(&accepted) && atomic_load(&attempts) == 2);
    puts("PASS stop during USB-online wait");

    reset();
    assert(hv_g4_ui_start_allowed(&stop_state));
    atomic_store(&session_active, 1);
    hv_g4_ui_request_stop(&stop_state, fake_cancel);
    hv_g4_ui_after_start(&stop_state, fake_cancel);
    assert(atomic_load(&accepted));
    puts("PASS stop after session creation, before post-start check");

    reset();
    assert(hv_g4_ui_start_allowed(&stop_state));
    atomic_store(&session_active, 1);
    hv_g4_ui_after_start(&stop_state, fake_cancel);
    hv_g4_ui_request_stop(&stop_state, fake_cancel);
    assert(atomic_load(&accepted));
    puts("PASS stop after post-start check");

    reset();
    assert(hv_g4_ui_start_allowed(&stop_state));
    atomic_store(&session_active, 1);
    hv_g4_ui_after_start(&stop_state, fake_cancel);
    assert(!atomic_load(&accepted) && !atomic_load(&attempts));
    puts("PASS normal start without cancellation");
    test_threaded_race();
    return 0;
}
