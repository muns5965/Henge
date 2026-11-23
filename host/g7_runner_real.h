#ifndef HENGEVM_G7_RUNNER_REAL_H
#define HENGEVM_G7_RUNNER_REAL_H
/* The execution table of the G7 runner (host/g7_runner_real.c). Compiled ONLY into the execution build (tools/g7-runner-build.sh real). */
#include "runner_g7_core.h"
#include "g7_restore.h"
/* Fills *api. Verifies (hashes) the IPSW against ipsw_sha256 first and remembers the file's identity, so a later swap is noticed before the restore starts. 0 or -1. */
int g7_real_setup(const HvG7Request *r, const char *workdir, const char *ipsw_sha256, HvG7Api *api);
/* The time limits of the restore (hv_g7_limits_make); without this call the provisional defaults apply. */
void g7_real_set_limits(const HvG7RestoreLimits *lim);
void g7_real_end(void);
#endif
