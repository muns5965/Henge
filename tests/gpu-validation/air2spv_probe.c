/*
 * AIR -> SPIR-V probe for the libapplegfx-vulkan air2spv path.
 *
 * Usage:
 *   air2spv_probe <input.air> <outdir> [name:vertex|fragment ...]
 *
 * Steps (matching the A1-A2 criteria in component-validation.md):
 *   A1  lagfx_air_module_open on the input bytes; list module functions.
 *   A2a per named function: lagfx_air2spv_translate_function with an
 *       explicit stage. This path has no reference fallback: it either
 *       returns a SPIR-V blob or an error status.
 *   A2b lagfx_air2spv_translate_module (the product entry point). This path
 *       MAY fall back to reference stub SPIR-V. Fallback is detected by the
 *       caller from the log (see run_a2.sh): the markers are
 *       "falling back" (WARN) and "emitting reference" (TRACE).
 *
 * Writes each produced blob to <outdir>/<name>.spv and prints its size.
 * Blobs are only produced by this run; committed .spv files are never read.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air/bitcode_reader.h"
#include "air2spv/translate.h"
#include "air2spv/translate_function.h"

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static int write_blob(const char *dir, const char *name, const uint8_t *blob, size_t size) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.spv", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    size_t w = fwrite(blob, 1, size, f);
    fclose(f);
    return w == size ? 0 : -1;
}

static int find_fn(const lagfx_air_module_t *m, const char *name, uint32_t *idx_out) {
    uint32_t nf = 0;
    const lagfx_air_function_t *fns = lagfx_air_module_functions(m, &nf);
    for (uint32_t i = 0; i < nf; i++) {
        const char *nm = lagfx_air_module_string(m, fns[i].name_offset);
        if (nm && strcmp(nm, name) == 0) { *idx_out = i; return 0; }
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <input.air> <outdir> [name:vertex|fragment ...]\n", argv[0]);
        return 1;
    }
    size_t len = 0;
    uint8_t *buf = slurp(argv[1], &len);
    if (!buf) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }

    lagfx_air_module_t *m = NULL;
    lagfx_status_t st = lagfx_air_module_open(buf, len, &m);
    printf("A1 module_open: status=%d module=%s bytes=%zu\n", (int)st, m ? "ok" : "null", len);
    if (st != LAGFX_OK || !m) return 3;

    uint32_t nf = 0;
    const lagfx_air_function_t *fns = lagfx_air_module_functions(m, &nf);
    for (uint32_t i = 0; i < nf; i++) {
        const char *nm = lagfx_air_module_string(m, fns[i].name_offset);
        printf("fn[%u] name=%s proto=%d body_offset=%u\n", i, nm ? nm : "(null)",
               (int)fns[i].is_proto, (unsigned)fns[i].body_offset);
    }

    int rc = 0;
    for (int a = 3; a < argc; a++) {
        char *spec = malloc(strlen(argv[a]) + 1); strcpy(spec, argv[a]);
        char *colon = strchr(spec, ':');
        if (!colon) { fprintf(stderr, "bad spec %s\n", argv[a]); free(spec); return 1; }
        *colon = '\0';
        const char *fname = spec, *sname = colon + 1;
        lagfx_xlate_stage_t stage;
        if (strcmp(sname, "vertex") == 0) stage = LAGFX_XLATE_STAGE_VERTEX;
        else if (strcmp(sname, "fragment") == 0) stage = LAGFX_XLATE_STAGE_FRAGMENT;
        else { fprintf(stderr, "bad stage %s\n", sname); free(spec); return 1; }

        uint32_t idx = 0;
        if (find_fn(m, fname, &idx) != 0) {
            printf("A2a %s: function not found in module\n", fname);
            rc = 4; free(spec); continue;
        }
        uint8_t *blob = NULL; size_t size = 0;
        lagfx_status_t ts = lagfx_air2spv_translate_function(m, idx, stage, &blob, &size);
        printf("A2a translate_function %s (%s): status=%d size=%zu\n",
               fname, sname, (int)ts, size);
        if (ts == LAGFX_OK && blob) {
            if (write_blob(argv[2], fname, blob, size) != 0) rc = 5;
            free(blob);
        } else {
            rc = 6;
        }
        free(spec);
    }

    /* A2b: product entry point. Its log markers reveal stub fallback. */
    uint8_t *mblob = NULL; size_t msize = 0;
    lagfx_status_t ms = lagfx_air2spv_translate_module(m, &mblob, &msize);
    printf("A2b translate_module: status=%d size=%zu\n", (int)ms, msize);
    if (ms == LAGFX_OK && mblob) {
        write_blob(argv[2], "module_path", mblob, msize);
        free(mblob);
    }

    lagfx_air_module_free(m);
    free(buf);
    return rc;
}
