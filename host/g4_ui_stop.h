#ifndef HV_G4_UI_STOP_H
#define HV_G4_UI_STOP_H

/* G4 UI diagnostic only. Keep the stop request across the interval before
   hv_g4_start() has created the restore session. The caller supplies the
   restore-session cancellation function; this header performs no I/O. */
#include <stdatomic.h>

typedef struct { atomic_int requested; } HvG4UiStop;

static inline int hv_g4_ui_start_allowed(const HvG4UiStop *s) {
    return !atomic_load(&s->requested);
}
static inline void hv_g4_ui_request_stop(HvG4UiStop *s, void (*cancel)(void)) {
    atomic_store(&s->requested, 1);
    cancel(); /* best effort: before the session exists, this may be refused */
}
static inline void hv_g4_ui_after_start(const HvG4UiStop *s, void (*cancel)(void)) {
    if (atomic_load(&s->requested)) cancel();
}

#endif
