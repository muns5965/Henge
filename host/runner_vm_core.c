#define _GNU_SOURCE
#include "runner_vm_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

struct HvRunner {
    const HvRunnerApi *api;
    HvShm *shm;
    HvDispBuf *db;
    int ready;
    HvRunnerStats st;
    uint8_t *bounce;            /* our own copy of the picture, tightly packed: what the shared file receives */
    uint32_t bw, bh;
    int fetch_error;
};

HvRunner *hv_runner_create(const HvRunnerApi *api, HvShm *shm) {
    if (!api || !shm) return NULL;
    HvRunner *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->api = api; r->shm = shm;
    r->db = hv_dispbuf_create();
    r->bounce = malloc(HV_SHM_PIXEL_BYTES);
    if (!r->db || !r->bounce) { if (r->db) hv_dispbuf_destroy(r->db); free(r->bounce); free(r); return NULL; }
    return r;
}

void hv_runner_destroy(HvRunner *r) {
    if (!r) return;
    hv_dispbuf_destroy(r->db);
    free(r->bounce);
    free(r);
}

void hv_runner_set_ready(HvRunner *r, int ready) { r->ready = ready ? 1 : 0; }
void hv_runner_stats(const HvRunner *r, HvRunnerStats *out) { *out = r->st; }
void hv_runner_close_display(HvRunner *r) { hv_dispbuf_close(r->db); }

/* ---- commands ---- */
static int in_range(int v, int lo, int hi) { return v >= lo && v <= hi; }

int hv_runner_cmd_valid(const HvShmCmd *c) {
    switch (c->type) {
    case HV_SHM_CMD_KEY: return in_range(c->a, 1, 0x2ff);                       /* Linux input codes (KEY_MAX = 0x2ff); b = down */
    case HV_SHM_CMD_MOVE: return in_range(c->a, -32768, 32767) && in_range(c->b, -32768, 32767);
    case HV_SHM_CMD_BUTTON: return in_range(c->a, 0, 2);                         /* 0 left, 1 right, 2 middle; b = down */
    case HV_SHM_CMD_WHEEL:
    case HV_SHM_CMD_HWHEEL: return in_range(c->a, -1024, 1024);
    default: return 0;
    }
}

static void apply(HvRunner *r, const HvShmCmd *c) {
    r->st.cmds_popped++;
    if (!hv_runner_cmd_valid(c)) { r->st.cmds_rejected++; return; }
    if (!r->ready) { r->st.cmds_skipped++; return; }
    switch (c->type) {
    case HV_SHM_CMD_KEY: r->api->key((uint16_t)c->a, c->b != 0); break;
    case HV_SHM_CMD_MOVE: r->api->mouse_move(c->a, c->b); break;
    case HV_SHM_CMD_BUTTON: r->api->mouse_button(c->a, c->b != 0); break;
    case HV_SHM_CMD_WHEEL: r->api->mouse_wheel(c->a); break;
    case HV_SHM_CMD_HWHEEL: r->api->mouse_hwheel(c->a); break;
    }
    r->st.calls_made++;
    r->st.cmds_dispatched++;
}

int hv_runner_pump_cmds(HvRunner *r) {
    int total = 0;
    for (;;) {
        HvShmCmd cmds[64];
        int n = hv_shm_pop_cmds(r->shm, cmds, 64);
        if (n < 0) { r->st.shm_errors++; return total ? total : -1; }
        if (n == 0) return total;
        for (int i = 0; i < n; i++) apply(r, &cmds[i]);
        total += n;
    }
}

/* ---- pointer ---- */
int hv_runner_pump_pointer(HvRunner *r) {
    int x = -1, y = -1;
    r->st.queries_made++;
    int rc = r->api->pointer(&x, &y);
    if (rc != 0) { r->st.pointer_absent++; hv_shm_set_pointer(r->shm, 0, 0, 0); return 0; }
    if (!in_range(x, 0, 65535) || !in_range(y, 0, 65535)) {   /* the guest driver writes 16 bits each */
        r->st.call_errors++; hv_shm_set_pointer(r->shm, 0, 0, 0); return -1;
    }
    r->st.pointer_valid++;
    hv_shm_set_pointer(r->shm, 1, x, y);
    return 1;
}

/* ---- picture ---- */
/* Runs inside hv_dispbuf_pull, under its lock: the only place the machine's pointer into guest memory is used. It is copied at once into our own memory. */
static int fetch_cb(void *ctx, const uint8_t **pixels, uint32_t *width, uint32_t *height, uint32_t *stride_bytes) {
    HvRunner *r = ctx;
    HvBootDisplay d;
    memset(&d, 0, sizeof d);
    r->st.queries_made++;
    if (r->api->boot_display(&d) != 0) { r->st.display_absent++; return -1; }
    uint64_t need = (uint64_t)d.width * 4u;
    if (!d.pixels || !d.width || !d.height || d.width > HV_DISP_MAX_DIM || d.height > HV_DISP_MAX_DIM || d.stride < need || need * d.height > HV_SHM_PIXEL_BYTES) {
        r->st.call_errors++; r->fetch_error = 1; return -1;
    }
    for (uint32_t y = 0; y < d.height; y++) memcpy(r->bounce + (size_t)y * need, d.pixels + (size_t)y * d.stride, (size_t)need);
    r->bw = d.width; r->bh = d.height;
    r->st.display_present++;
    *pixels = r->bounce; *width = d.width; *height = d.height; *stride_bytes = (uint32_t)need;
    return 0;
}

int hv_runner_pump_frame(HvRunner *r) {
    r->fetch_error = 0;
    int rc = hv_dispbuf_pull(r->db, fetch_cb, r);
    if (rc == HV_DISP_NEW) {
        int s = hv_shm_publish(r->shm, r->bounce, r->bw, r->bh, r->bw * 4u);
        if (s == 1) r->st.frames_published++; else r->st.shm_errors++;
    } else if (rc == HV_DISP_REJECTED) r->st.call_errors++;
    return rc;
}

/* ---- safety ---- */
void hv_runner_make_config(HvVmConfig *c, const char *rom, const char *disk, const char *aux, uint64_t ram_mb, int ncpus, size_t jit_mb) {
    memset(c, 0, sizeof *c);
    c->rom = rom; c->disk = disk; c->aux = aux;
    c->aux_offset = 16384;
    c->ram_mb = ram_mb; c->ncpus = ncpus; c->jit_bytes = jit_mb << 20;
    c->no_sound = 1;
    /* the four settings this runner exists to guarantee: */
    c->readonly_storage = 1;
    c->no_net = 1;
    c->installer = 0;
    c->ecid = 0; c->seed = 0;
}

int hv_runner_config_ok(const HvVmConfig *c, char *why, size_t cap) {
#define BAD(msg) do { if (why && cap) snprintf(why, cap, "%s", msg); return -1; } while (0)
    if (!c) BAD("no configuration");
    if (!c->readonly_storage) BAD("storage is not read-only");
    if (!c->no_net) BAD("the guest network is not off");
    if (c->installer) BAD("installer boot is on");
    if (c->ecid || c->seed) BAD("a machine identity is set");
    if (c->jit_rx || c->jit_rw) BAD("host-provided JIT memory");
    if (!c->rom || !c->disk || !c->aux || !*c->rom || !*c->disk || !*c->aux) BAD("a path is missing");
    if (c->ram_mb < 64 || c->ram_mb > 4096) BAD("RAM size out of range (64..4096 MB)");
    if (c->ncpus < 1 || c->ncpus > 4) BAD("CPU count out of range (1..4)");
    if (c->jit_bytes < ((size_t)16 << 20) || c->jit_bytes > ((size_t)768 << 20)) BAD("JIT size out of range (16..768 MB)");
    if (why && cap) why[0] = 0;
    return 0;
#undef BAD
}

void hv_runner_config_line(const HvVmConfig *c, char *out, size_t cap) {
    snprintf(out, cap, "readonly_storage=%d no_net=%d no_sound=%d installer=%d identity=%s host_jit=%s ram_mb=%llu cpus=%d jit_mb=%zu aux_offset=%llu",
             c->readonly_storage, c->no_net, c->no_sound, c->installer, (c->ecid || c->seed) ? "SET" : "none", (c->jit_rx || c->jit_rw) ? "SET" : "none",
             (unsigned long long)c->ram_mb, c->ncpus, c->jit_bytes >> 20, (unsigned long long)c->aux_offset);
}

int hv_runner_scrub_env(char *names, size_t cap) {
    int total = 0, n;
    if (names && cap) names[0] = 0;
    do {   /* a pass takes at most 256 names; go again until none is left */
        char *found[256];
        n = 0;
        for (char **e = environ; e && *e && n < 256; e++)
            if (!strncmp(*e, "HV_", 3)) {
                const char *eq = strchr(*e, '=');
                size_t len = eq ? (size_t)(eq - *e) : strlen(*e);
                char *nm = malloc(len + 1);
                if (!nm) continue;
                memcpy(nm, *e, len); nm[len] = 0;
                found[n++] = nm;
            }
        for (int i = 0; i < n; i++) {
            if (names && cap > strlen(names) + strlen(found[i]) + 2) { if (names[0]) strcat(names, ","); strcat(names, found[i]); }
            unsetenv(found[i]);
            free(found[i]);
        }
        total += n;
    } while (n == 256);
    return total;
}
