/* Isolated stop-request state tests. No VM, no restore stack, no network. */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
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
    return 0;
}
