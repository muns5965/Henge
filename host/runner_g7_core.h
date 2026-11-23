#ifndef HENGEVM_RUNNER_G7_CORE_H
#define HENGEVM_RUNNER_G7_CORE_H
/* The G7 runner's decision core: the one place that turns a request into (at most) a writable VM configuration. A SKELETON: it is not linked into any shipped runner,
   nothing in the app calls it, and in every build that is not a test build the execution stage does not exist (it is compiled out, see HV_G7_MOCK_EXECUTION below).

   Order, each stage refuses before the next one can run:
     CONFIG     hv_g7_make_config builds the HvVmConfig; hv_g7_config_ok (host/g7_preflight.h) must accept it
     PREFLIGHT  hv_g7_preflight (read-only): disk, AUX, ECID, IPSW, space, a clean start
     SWITCH     HV_G7_EXECUTION_ENABLED: 0 in every shipped build, so the run ends here with verdict BLOCKED_BY_SWITCH and nothing in the directory has changed
   ---- only when compiled with -DHV_G7_MOCK_EXECUTION (tools/g7-preflight-test.sh, with a recorder in place of the machine; no shipped build may define it) ----
     PAIR       api->pair_begin (the caller hands in the disk creator; none is wired to any app)
     VM         config re-validated, then api->create_vm(cfg). The shipped runner has no such table: nothing here can reach hv_vm_create
     RESTORE    api->start_restore
   WHAT HAS TO CHANGE TO MAKE G7 REAL (each is a separately approved step; until then every item below stays as it is):
     1. HV_G7_EXECUTION_ENABLED / HV_G7_MOCK_EXECUTION: the literal switch and the compiled-out stages in host/runner_g7_core.c, and the guard in tools/check-no-test-flags.sh
     2. a G7 runner program (like host/ui_runner_vm_g4.c, its own binary) whose api->create_vm calls hv_vm_create ONLY after hv_g7_config_ok accepted the very same structure
     3. api->pair_begin: the real disk creator handed in (tools/check-no-test-flags.sh then flags the call site on purpose), and the 128 GiB disk it makes
     4. a build without HV_STOP_AFTER_RESTORE_OPTIONS (third_party/idevicerestore/src/restore.c, host/install_g6.c) so restored_start_restore() is reachable, and api->start_restore
        calling hv_install_start (host/install.c)
     5. host/vm.c 319-320: disk and AUX share one readonly_storage flag; G7 opens both read-write (a readonly_aux split only if a run shows it is needed)
     6. a launcher (Java, START_ENABLED) and the ECID hand-over; the restore-complete judgment that is the only thing allowed to call hv_g7_pair_finish
   The existing hv_runner_config_ok (host/runner_vm_core.c) and the G0-G6 programs are not touched by any of this and keep refusing writable storage. */
#include <stddef.h>
#include <stdint.h>

#include "g7_preflight.h"
#include "vm.h"

#ifdef HV_G7_MOCK_EXECUTION
#define HV_G7_EXECUTION_ENABLED 1
#else
#define HV_G7_EXECUTION_ENABLED 0
#endif

typedef struct {
    const char *dir, *ipsw_dir, *ecid_dir;   /* as HvG7Input */
    const char *rom;
    uint64_t ecid, ram_mb;
    int ncpus;
    size_t jit_mb;
} HvG7Request;

typedef struct {
    int (*pair_begin)(const char *dir, char *note, int cap);
    int (*create_vm)(const HvVmConfig *cfg, char *err, size_t errlen);
    int (*start_restore)(void);
} HvG7Api;

typedef enum { HV_G7_STAGE_CONFIG = 0, HV_G7_STAGE_PREFLIGHT, HV_G7_STAGE_SWITCH, HV_G7_STAGE_PAIR, HV_G7_STAGE_VM, HV_G7_STAGE_RESTORE, HV_G7_STAGE_DONE } HvG7Stage;
typedef enum { HV_G7_REFUSED = 0, HV_G7_BLOCKED_BY_SWITCH, HV_G7_FAILED, HV_G7_RAN } HvG7Verdict;
typedef struct { HvG7Stage stage; HvG7Verdict verdict; char reason[240]; HvG7Report preflight; } HvG7Result;

/* Fills *c for this request. disk/aux receive the two paths (c points into them). Returns 0, or -1 when a path does not fit. Does not validate. */
int hv_g7_make_config(HvVmConfig *c, const HvG7Request *r, char *disk, size_t disk_cap, char *aux, size_t aux_cap);
/* The whole decision. api may be NULL (nothing can be called then). Never creates, opens or changes a file in a build without HV_G7_MOCK_EXECUTION. */
int hv_g7_run(const HvG7Request *r, const HvG7Api *api, HvG7Result *out);

#endif
