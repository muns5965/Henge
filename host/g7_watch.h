#ifndef HENGEVM_G7_WATCH_H
#define HENGEVM_G7_WATCH_H
/* The G7 runner's watchdog lines (compiled into host/g7_runner_real.c only; header-only so that tests/test_g7_watch.c can include the same code).
   Every 30 s while the restore runs the runner writes ONE line to stderr, built from a sample of read-only counters:
     g7-alive: t=<s>s guest_blocks=cpu0:<n>,cpu1:<n> disk_done=<n> disk_inflight=<n> usb_cmds=<n> progress=yes|no
   and when the counters have not moved for two samples in a row (60 s or more) the line is
     g7-stall: quiet=<s>s guest_blocks=... disk_done=... disk_inflight=<n> disk_last=<read|write|flush|other>@<s>s usb_cmds=... hint=<text>
   The hint separates the two cases the log alone cannot: a disk request that started long ago and never finished (the host may be stuck in storage), or nothing in flight (the guest
   is idle or waiting for something the host has not sent). Numbers and fixed words only: no path, no identifier, no URL. It reports; it changes nothing. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define HV_G7W_TICK_S 30        /* seconds between samples */
#define HV_G7W_STALL_TICKS 2    /* samples without any movement before the line becomes g7-stall */

typedef struct {
    uint64_t t_s;               /* seconds since the restore began */
    uint64_t blocks[2];         /* blocks dispatched by cpu 0 and cpu 1 */
    uint64_t disk_done, disk_inflight, disk_last_age_s;
    int disk_last_type;         /* virtio-blk type: 0 read, 1 write, 4 flush */
    uint64_t usb_cmds;
} HvG7WatchSample;

typedef struct { int have_prev; HvG7WatchSample prev; unsigned quiet_ticks; uint64_t quiet_since_s; } HvG7WatchState;

static inline const char *hv_g7w_type(int t) { return t == 0 ? "read" : t == 1 ? "write" : t == 4 ? "flush" : "other"; }

/* Always fills `line` (one line, ends in '\n') and returns 1 when it is a g7-stall line, else 0. */
static inline int hv_g7w_step(HvG7WatchState *st, const HvG7WatchSample *s, char *line, size_t cap) {
    int moved = !st->have_prev || s->blocks[0] != st->prev.blocks[0] || s->blocks[1] != st->prev.blocks[1] || s->disk_done != st->prev.disk_done ||
                s->usb_cmds != st->prev.usb_cmds;
    if (moved) { st->quiet_ticks = 0; st->quiet_since_s = s->t_s; }
    else st->quiet_ticks++;
    st->prev = *s; st->have_prev = 1;
    if (st->quiet_ticks < HV_G7W_STALL_TICKS) {
        snprintf(line, cap, "g7-alive: t=%llus guest_blocks=cpu0:%llu,cpu1:%llu disk_done=%llu disk_inflight=%llu usb_cmds=%llu progress=%s\n",
                 (unsigned long long)s->t_s, (unsigned long long)s->blocks[0], (unsigned long long)s->blocks[1], (unsigned long long)s->disk_done,
                 (unsigned long long)s->disk_inflight, (unsigned long long)s->usb_cmds, moved ? "yes" : "no");
        return 0;
    }
    uint64_t quiet = s->t_s - st->quiet_since_s;
    const char *hint = s->disk_inflight > 0 && s->disk_last_age_s >= quiet
        ? "a disk request started before the quiet period and has not finished: the host may be stuck in storage"
        : s->disk_inflight > 0 ? "a disk request is in flight but started recently"
        : "no disk request in flight: the guest is idle or waiting for something the host has not sent";
    snprintf(line, cap, "g7-stall: quiet=%llus guest_blocks=cpu0:%llu,cpu1:%llu disk_done=%llu disk_inflight=%llu disk_last=%s@%llus usb_cmds=%llu hint=%s\n",
             (unsigned long long)quiet, (unsigned long long)s->blocks[0], (unsigned long long)s->blocks[1], (unsigned long long)s->disk_done,
             (unsigned long long)s->disk_inflight, hv_g7w_type(s->disk_last_type), (unsigned long long)s->disk_last_age_s, (unsigned long long)s->usb_cmds, hint);
    return 1;
}
#endif
