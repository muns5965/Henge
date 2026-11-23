#define _GNU_SOURCE
#include "runner_g7_core.h"

#include <stdio.h>
#include <string.h>

#define SET(res, st, vd, ...) do { (res)->stage = (st); (res)->verdict = (vd); snprintf((res)->reason, sizeof (res)->reason, __VA_ARGS__); } while (0)

int hv_g7_make_config(HvVmConfig *c, const HvG7Request *r, char *disk, size_t disk_cap, char *aux, size_t aux_cap) {
    memset(c, 0, sizeof *c);
    if (!r || !r->dir || snprintf(disk, disk_cap, "%s/%s", r->dir, "macos_disk.img") >= (int)disk_cap || snprintf(aux, aux_cap, "%s/%s", r->dir, HV_G7_AUX_NAME) >= (int)aux_cap) return -1;
    c->rom = r->rom; c->disk = disk; c->aux = aux;
    c->aux_offset = 16384;
    c->ecid = r->ecid; c->seed = 0;
    c->ram_mb = r->ram_mb; c->ncpus = r->ncpus; c->jit_bytes = r->jit_mb << 20;
    c->installer = 1; c->no_sound = 1;
    c->readonly_storage = 0;      /* only a configuration that hv_g7_config_ok accepts may carry this */
    return 0;
}

int hv_g7_run(const HvG7Request *r, const HvG7Api *api, HvG7Result *out) {
    HvG7Result local;
    if (!out) out = &local;
    memset(out, 0, sizeof *out);
    (void)api;
    char disk[4096], aux[4096], why[160];
    HvVmConfig cfg;
    if (!r || !r->dir || !r->ipsw_dir || !r->ecid_dir) { SET(out, HV_G7_STAGE_CONFIG, HV_G7_REFUSED, "incomplete request"); return -1; }
    if (hv_g7_make_config(&cfg, r, disk, sizeof disk, aux, sizeof aux) != 0) { SET(out, HV_G7_STAGE_CONFIG, HV_G7_REFUSED, "a path is too long"); return -1; }
    if (hv_g7_config_ok(&cfg, r->dir, r->ecid, why, sizeof why) != 0) { SET(out, HV_G7_STAGE_CONFIG, HV_G7_REFUSED, "configuration refused: %s", why); return -1; }

    HvG7Input in = {.dir = r->dir, .ipsw_dir = r->ipsw_dir, .ecid_dir = r->ecid_dir, .ecid = r->ecid};
    if (hv_g7_preflight(&in, &out->preflight) != 0) {
        const char *first = "see the report";
        for (int i = 0; i < out->preflight.n; i++) if (out->preflight.check[i].level == HV_G7_BLOCK) { first = out->preflight.check[i].item; break; }
        SET(out, HV_G7_STAGE_PREFLIGHT, HV_G7_REFUSED, "preflight blocked (%d item(s), first: %s)", out->preflight.blocked, first);
        return -1;
    }
#if !HV_G7_EXECUTION_ENABLED
    SET(out, HV_G7_STAGE_SWITCH, HV_G7_BLOCKED_BY_SWITCH, "the request is valid; G7 execution is not enabled in this build, so nothing was created, opened or started");
    return -1;
#else
    if (!api || !api->pair_begin || !api->create_vm || !api->start_restore) { SET(out, HV_G7_STAGE_SWITCH, HV_G7_REFUSED, "the execution table is incomplete"); return -1; }
    char note[200];
    if (api->pair_begin(r->dir, note, sizeof note) != 0) { SET(out, HV_G7_STAGE_PAIR, HV_G7_FAILED, "the pair was not made: %s", note); return -1; }
    /* the structure is validated again right before it is handed on (the state of the directory has changed since the first check) */
    if (hv_g7_config_ok(&cfg, r->dir, r->ecid, why, sizeof why) != 0) { SET(out, HV_G7_STAGE_VM, HV_G7_REFUSED, "configuration refused before the machine: %s", why); return -1; }
    char err[200] = "";
    if (api->create_vm(&cfg, err, sizeof err) != 0) { SET(out, HV_G7_STAGE_VM, HV_G7_FAILED, "the machine was not created: %s", err); return -1; }
    if (api->start_restore() != 0) { SET(out, HV_G7_STAGE_RESTORE, HV_G7_FAILED, "the restore did not start"); return -1; }
    SET(out, HV_G7_STAGE_DONE, HV_G7_RAN, "run through the recorder");
    return 0;
#endif
}
