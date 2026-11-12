/* Henge: the ONE interface between the vendored restore stack (idevicerestore, libimobiledevice, reverse proxy) and the host session lifetime registry
   (docs/i3-i6-contract-2026-11-12.md). I3 (async data request), I5 (idevice connections) and I6 (reverse proxy threads and sockets) all use THIS file and nothing
   else; none of them may keep a private reference or ownership scheme (two schemes for one fd means a double close).
   NULL ops (no host session) = every helper below does nothing and reports "allowed": the original behaviour is unchanged. */
#ifndef HV_LIFE_H
#define HV_LIFE_H
#include <stdint.h>

enum { HV_LIFE_K_ASYNC = 0, HV_LIFE_K_PROXY_CTRL = 1, HV_LIFE_K_PROXY_CONN = 2 };   /* same values as HvRegKind in host/restore_registry.h */

typedef struct HvLifeOps {
	int (*task_try_register)(int kind, uint64_t *gen);                       /* 0 = registered (counter++), gen = the generation to complete with; -1 = refused */
	void (*task_complete)(uint64_t gen, int kind, int64_t failure);          /* the task's LAST act (after every shared object was touched for the last time) */
	void (*task_unregister_failed)(uint64_t gen, int kind, int64_t failure); /* thread creation failed after try_register */
	int (*terminate_requested)(void);                                        /* 1 = the host asked the work to stop (cancel or close) */
	int (*conn_register)(int fd, uint64_t *gen);                             /* 0 = registered (the host may now shutdown() it from another thread); -1 = refused: do not use the fd */
	int (*conn_unregister_wait)(uint64_t gen, int fd);                       /* 0 = every reference came back, the caller may close(fd); -1 = it may NOT close(fd) */
} HvLifeOps;

void hv_life_install(const HvLifeOps *ops);   /* host only; NULL removes */

/* helpers for the vendored side. With no ops installed they return the "allowed" value. */
int hv_life_task_begin(int kind, uint64_t *gen);                           /* 0 ok (or no ops), -1 refused: do not create the thread */
void hv_life_task_end(uint64_t gen, int kind, int64_t failure);            /* completion; no-op without ops */
void hv_life_task_create_failed(uint64_t gen, int kind, int64_t failure);  /* no-op without ops */
int hv_life_installed(void);                                                /* 1 when a host session installed ops (I3 6-2 only) */
int hv_life_terminate_requested(void);                                      /* 0 without ops */
int hv_life_conn_add(int fd, uint64_t *gen);                                /* 0 ok (or no ops); -1: do not use the connection */
int hv_life_conn_remove(uint64_t gen, int fd);                              /* 0: close(fd) allowed (always without ops); -1: leave the fd open */
#endif
