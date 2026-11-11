/* Henge: see hv_life.h. The pointer is read once per call so an install/removal by the host cannot change the table under a caller. */
#include "hv_life.h"
#include <stddef.h>

static const HvLifeOps *volatile g_ops;

void hv_life_install(const HvLifeOps *ops) { __atomic_store_n(&g_ops, ops, __ATOMIC_SEQ_CST); }
static const HvLifeOps *ops(void) { return __atomic_load_n(&g_ops, __ATOMIC_SEQ_CST); }

int hv_life_task_begin(int kind, uint64_t *gen) { const HvLifeOps *o = ops(); if (gen) *gen = 0; return (o && o->task_try_register) ? o->task_try_register(kind, gen) : 0; }
void hv_life_task_end(uint64_t gen, int kind, int64_t failure) { const HvLifeOps *o = ops(); if (o && gen && o->task_complete) o->task_complete(gen, kind, failure); }
void hv_life_task_create_failed(uint64_t gen, int kind, int64_t failure) { const HvLifeOps *o = ops(); if (o && gen && o->task_unregister_failed) o->task_unregister_failed(gen, kind, failure); }
int hv_life_terminate_requested(void) { const HvLifeOps *o = ops(); return (o && o->terminate_requested) ? o->terminate_requested() : 0; }
int hv_life_conn_add(int fd, uint64_t *gen) { const HvLifeOps *o = ops(); if (gen) *gen = 0; return (o && o->conn_register) ? o->conn_register(fd, gen) : 0; }
int hv_life_conn_remove(uint64_t gen, int fd) { const HvLifeOps *o = ops(); return (o && gen && o->conn_unregister_wait) ? o->conn_unregister_wait(gen, fd) : 0; }
