#ifndef HENGEVM_RENDER_GATE_H
#define HENGEVM_RENDER_GATE_H
/* C0 fix: the lifetime gate of a render thread's resources (pure C, no JNI, so Linux tests can race it).

   The problem it solves: a thread uses resources (the screen buffer, the synthetic source picture) that another thread frees. "Is anything using them?" and "free them" must be
   ONE decision under ONE lock, with new users kept out by the same lock; a counter that is checked and then acted on in two steps lets a user in between.

   States (all changes under the gate's mutex):
     IDLE       nothing exists. Only start may leave it.
     STARTING   begin_start succeeded; the owner is building the resources. Nobody may enter.
     OPEN       open() was called; enter() is allowed.
     CLOSING    begin_close() was called; enter() is refused; users that are already inside finish.
     RELEASING  try_destroy() returned 1 to ONE caller, who now frees the resources. Nobody may enter; no other try_destroy gets the right.
   back to IDLE  when that caller calls released(): only then can a new start begin. A state change alone does not mean the memory is gone.

   Rules: every enter() that returned 1 is paired with exactly one leave(). The resources are used only between them. The one that frees them is the caller whose try_destroy()
   returned 1, and no one else. */
#include <pthread.h>

typedef struct HvGate {
    pthread_mutex_t mu;
    int state;
    int active;
} HvGate;

enum { HV_GATE_IDLE = 0, HV_GATE_STARTING, HV_GATE_OPEN, HV_GATE_CLOSING, HV_GATE_RELEASING };

void hv_gate_init(HvGate *g);                 /* IDLE, nobody inside */
void hv_gate_fini(HvGate *g);

/* IDLE -> STARTING: 1, the caller builds the resources and then calls hv_gate_open (or hv_gate_start_failed). 0 if already started (OPEN: the caller may treat it as "running").
   -1 if a previous instance is still closing or being released (the caller must not build a new one). */
int hv_gate_begin_start(HvGate *g);
void hv_gate_open(HvGate *g);                 /* STARTING -> OPEN */
void hv_gate_start_failed(HvGate *g);         /* STARTING -> IDLE (nothing was built, or the caller already freed it) */

/* 1: inside (the resources may be used until leave). 0: refused (not OPEN). */
int hv_gate_enter(HvGate *g);
void hv_gate_leave(HvGate *g);

/* OPEN -> CLOSING. Idempotent; in any other state it changes nothing. Returns the state after the call. */
int hv_gate_begin_close(HvGate *g);

/* The only way to the right to free. CLOSING (or OPEN, which it closes first) with nobody inside -> RELEASING and returns 1: this caller frees the resources and then calls
   hv_gate_released. Returns 0 when users are still inside (try again later: nothing changed except that new entries are already refused) and -1 when the caller does not get the
   right: another caller has it (RELEASING), or there is nothing to free (IDLE or STARTING). */
int hv_gate_try_destroy(HvGate *g);
/* RELEASING -> IDLE, by the one that freed. */
void hv_gate_released(HvGate *g);

int hv_gate_state(HvGate *g);
int hv_gate_active(HvGate *g);

#endif
