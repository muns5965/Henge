#ifndef HENGEVM_FRAME_SHM_H
#define HENGEVM_FRAME_SHM_H
/* C1-1: what the app process and the runner process share. One file, mapped by both (MAP_SHARED), in the app's own folder:
     a header (page-aligned, 16 KB), then the newest picture.
   It carries four things, and everything in it is guarded by ONE lock, a word in the file that holds the holder's pid (see lock() in frame_shm.c):
     frame      the runner publishes a picture (BGRA, tightly packed); the app copies it out into memory of its own and draws from the copy
     commands   the app pushes input (key / mouse move / button / wheel / hwheel) into a ring; the runner pops them and hands them to the machine
     pointer    where the guest says its pointer is (valid = 0 until it says): the app's touch loop reads it as its feedback
     state      the runner's life: starting, running, stopping, stopped, plus a heartbeat the app can watch
   THE CONTRACT
     1. The lock is the only synchronisation. The picture is copied IN and OUT under it (a copy of ~4 MB takes a millisecond or two); nobody keeps a pointer into the
        mapping after unlocking. There is no lock-free reading of pixels, on purpose: a sequence number checked around a plain copy would still be a data race on the pixels.
     2. If a process dies while holding the lock (its pid is gone), the next locker takes it over, counts it (`recovered`), and DROPS the picture
        (width = 0: "no frame") because it may be half written. A holder that is alive is never taken over. The command ring cannot be half written (indices move last) but a command being pushed may be lost.
     3. A lock that cannot be taken within HV_SHM_LOCK_MS gives up (returns an error, nothing was touched): neither side can be frozen by the other.
     4. The guest's own framebuffer is NOT in this file and never leaves the runner: the runner copies it inside host/display_buf.h's pull (that module's contract holds
        there: the guest pointer lives only in the fetch callback), then publishes its own copy here. The mapped picture is a copy of a copy.
     5. Sizes: the picture is at most HV_SHM_PIXEL_BYTES; a larger one is refused and counted. The ring holds HV_SHM_CMD_SLOTS commands; when it is full the new command
        is dropped and counted (a touch screen can wait, a runner that stalls must not make the app block).
     6. Who writes what: the runner creates the file (hv_shm_create), writes frames, pointer, state and heartbeat, and only pops commands. The app opens it (hv_shm_open),
        only reads frames, pointer and state, and only pushes commands and sets `app_closed`.
   Why not a pthread mutex: Android's libc has no robust mutexes, and a mutex held by a killed process would block the other side for good. The word lock works the same on Android
   and Linux and needs nothing but atomics, kill(pid, 0) and a sleep. */
#include <stddef.h>
#include <stdint.h>

#define HV_SHM_PIXEL_BYTES (16u << 20)
#define HV_SHM_CMD_SLOTS 512u
#define HV_SHM_LOCK_MS 100

enum { HV_SHM_CMD_KEY = 1, HV_SHM_CMD_MOVE = 2, HV_SHM_CMD_BUTTON = 3, HV_SHM_CMD_WHEEL = 4, HV_SHM_CMD_HWHEEL = 5 };
typedef struct { uint32_t type; int32_t a, b; } HvShmCmd;   /* KEY: code, down. MOVE: dx, dy. BUTTON: button, down. WHEEL / HWHEEL: notches, 0 */

enum { HV_SHM_NONE = 0, HV_SHM_STARTING = 1, HV_SHM_RUNNING = 2, HV_SHM_STOPPING = 3, HV_SHM_STOPPED = 4 };

typedef struct HvShm HvShm;

/* The runner: creates (or truncates) the file at `path`, maps it, initialises it. 0, or -1 (errno set). The file is made with mode 0600 and without following a symlink. */
int hv_shm_create(const char *path, HvShm **out);
/* The app: opens an existing file made by hv_shm_create and checks it (magic, version, size). 0, or -1: not there, wrong, or not initialised yet. */
int hv_shm_open(const char *path, HvShm **out);
/* Unmaps. Nobody may be inside any call on it. */
void hv_shm_close(HvShm *s);

/* ---- runner ---- */
/* Copies a BGRA picture (row length stride bytes, at least width * 4) in, tightly packed, and bumps the frame number. 1 published; 0 refused (size 0, row too short, over
   HV_SHM_PIXEL_BYTES, overflow: counted, the old picture stays); -1 the lock could not be taken. */
int hv_shm_publish(HvShm *s, const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t stride);
/* Pops up to max commands into out. Returns how many (0 when none), or -1 if the lock could not be taken. */
int hv_shm_pop_cmds(HvShm *s, HvShmCmd *out, int max);
void hv_shm_set_pointer(HvShm *s, int valid, int x, int y);
void hv_shm_set_state(HvShm *s, int state);
void hv_shm_heartbeat(HvShm *s);          /* the runner calls it about every frame; not locked */
int hv_shm_app_closed(HvShm *s);          /* the app asked it to stop (set by hv_shm_app_close) */

/* ---- app ---- */
/* If a picture newer than *seen exists: copies it into dst (capacity cap bytes), sets *seen and the size, returns 1. 0 nothing newer. -1 no picture, or dst too small, or
   the lock could not be taken (nothing copied; *seen unchanged). */
int hv_shm_read_frame(HvShm *s, uint64_t *seen, uint8_t *dst, size_t cap, uint32_t *width, uint32_t *height);
/* Appends a command. 1 queued, 0 the ring was full (dropped, counted), -1 the lock could not be taken. */
int hv_shm_push_cmd(HvShm *s, uint32_t type, int a, int b);
/* The guest pointer as the runner last set it: 1 valid with x, y; 0 not valid; -1 the lock could not be taken (outputs untouched). */
int hv_shm_get_pointer(HvShm *s, int *x, int *y);
void hv_shm_app_close(HvShm *s);

typedef struct {
    int state;                  /* HV_SHM_* */
    uint32_t runner_pid;
    uint64_t heartbeat;         /* grows while the runner is alive */
    uint64_t frames, rejected;  /* published, refused */
    uint64_t cmds_pushed, cmds_popped, cmds_dropped;   /* popped = taken out of the ring by the runner; NOT "the machine did it" (the runner counts its own calls) */
    uint32_t recovered;         /* times a dead owner's lock was recovered */
    uint32_t width, height;     /* of the picture now held, 0 = none */
} HvShmStats;
int hv_shm_stats(HvShm *s, HvShmStats *out);   /* 0, or -1 when the lock could not be taken */

#endif
