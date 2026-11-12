#include "render_gate.h"

void hv_gate_init(HvGate *g) { pthread_mutex_init(&g->mu, NULL); g->state = HV_GATE_IDLE; g->active = 0; }
void hv_gate_fini(HvGate *g) { pthread_mutex_destroy(&g->mu); }

int hv_gate_begin_start(HvGate *g) {
    int r;
    pthread_mutex_lock(&g->mu);
    if (g->state == HV_GATE_IDLE) { g->state = HV_GATE_STARTING; r = 1; }
    else if (g->state == HV_GATE_OPEN) r = 0;
    else r = -1;                       /* STARTING by another caller, CLOSING, RELEASING: not now */
    pthread_mutex_unlock(&g->mu);
    return r;
}

void hv_gate_open(HvGate *g) {
    pthread_mutex_lock(&g->mu);
    if (g->state == HV_GATE_STARTING) g->state = HV_GATE_OPEN;
    pthread_mutex_unlock(&g->mu);
}

void hv_gate_start_failed(HvGate *g) {
    pthread_mutex_lock(&g->mu);
    if (g->state == HV_GATE_STARTING) g->state = HV_GATE_IDLE;
    pthread_mutex_unlock(&g->mu);
}

int hv_gate_enter(HvGate *g) {
    int r = 0;
    pthread_mutex_lock(&g->mu);
    if (g->state == HV_GATE_OPEN) { g->active++; r = 1; }
    pthread_mutex_unlock(&g->mu);
    return r;
}

void hv_gate_leave(HvGate *g) {
    pthread_mutex_lock(&g->mu);
    if (g->active > 0) g->active--;
    pthread_mutex_unlock(&g->mu);
}

int hv_gate_begin_close(HvGate *g) {
    pthread_mutex_lock(&g->mu);
    if (g->state == HV_GATE_OPEN) g->state = HV_GATE_CLOSING;
    int s = g->state;
    pthread_mutex_unlock(&g->mu);
    return s;
}

int hv_gate_try_destroy(HvGate *g) {
    int r;
    pthread_mutex_lock(&g->mu);
    if (g->state == HV_GATE_OPEN) g->state = HV_GATE_CLOSING;
    if (g->state == HV_GATE_CLOSING) {
        if (g->active == 0) { g->state = HV_GATE_RELEASING; r = 1; }   /* the check and the take-over are one step under the lock */
        else r = 0;
    } else r = -1;
    pthread_mutex_unlock(&g->mu);
    return r;
}

void hv_gate_released(HvGate *g) {
    pthread_mutex_lock(&g->mu);
    if (g->state == HV_GATE_RELEASING) g->state = HV_GATE_IDLE;
    pthread_mutex_unlock(&g->mu);
}

int hv_gate_state(HvGate *g) { pthread_mutex_lock(&g->mu); int s = g->state; pthread_mutex_unlock(&g->mu); return s; }
int hv_gate_active(HvGate *g) { pthread_mutex_lock(&g->mu); int a = g->active; pthread_mutex_unlock(&g->mu); return a; }
