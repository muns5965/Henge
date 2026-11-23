#ifndef HENGEVM_G7_PREFLIGHT_H
#define HENGEVM_G7_PREFLIGHT_H
/* G7 safety layer, first part. NOTHING here starts a machine, opens a disk for the guest, runs the restore or talks to Apple, and nothing in the app calls it yet.

   1. hv_g7_preflight        read-only look at the install disk, the AUX, the ECID file and the IPSW; says PASS / BLOCK per item, creates and changes nothing.
   2. hv_g7_config_ok        the ONE place that may accept a writable storage configuration (readonly_storage=0). hv_runner_config_ok (G0-G6, [A] runner) keeps refusing it.
   3. hv_g7_pair_*           the disk and the AUX as ONE install state: begin / finish / quarantine. Nothing is ever deleted or overwritten; a failed pair is moved aside.
   The ECID value is never written into a note, a log line or a report: only "matches" / "does not match".

   Names (all in one private directory, owned by the caller and not writable by group/others):
     macos_disk.img        128 GiB sparse, host/disk_policy.h
     macos_aux.img         33,570,816 bytes, the 24-byte header of host/vmrun.c make_aux (see below)
     macos_aux.initial     a byte copy of the AUX taken when the pair was created (the state before any restore write)
     g7_state.txt          "state=in_progress" while a restore may be writing, "state=done" after a restore that was confirmed complete
     g7_quarantine_<tag>/  where a failed pair is moved (rename only) */
#include <stddef.h>
#include <stdint.h>

#include "vm.h"

#define HV_G7_AUX_NAME "macos_aux.img"
#define HV_G7_AUX_INITIAL_NAME "macos_aux.initial"
#define HV_G7_STATE_NAME "g7_state.txt"
#define HV_G7_QUARANTINE_PREFIX "g7_quarantine_"
#define HV_G7_AUX_BYTES 33570816ULL
#define HV_G7_AUX_HEADER_BYTES 24u
#define HV_G7_ROM_BYTES 214904ULL
#ifndef HV_G7_ROM_SHA256_HEX   /* only the tests define it (their stand-in ROM), never the app */
#define HV_G7_ROM_SHA256_HEX "fd4973e42e609751830df6f3dd2d2b2ad2309fe38f0ca0f5aba02dcbefe45f91"
#endif
#define HV_G7_IPSW_NAME "UniversalMac_15.6.1_24G90_Restore.ipsw"
#define HV_G7_IPSW_BYTES 16814137790ULL
/* ~15 GB are written by the restore into a sparse file, plus the AUX copy and the Mac's own scratch: refuse below 32 GiB free. A floor, not a guarantee. */
#ifndef HV_G7_MIN_FREE_BYTES   /* only the tests define it (smaller), never the app */
#define HV_G7_MIN_FREE_BYTES (32ULL << 30)
#endif
#define HV_G7_ECID_FILE "vm_ecid.txt"

typedef enum { HV_G7_PASS = 0, HV_G7_BLOCK = 1 } HvG7Level;
typedef struct { const char *item; HvG7Level level; char text[240]; } HvG7Check;
#define HV_G7_MAX_CHECKS 12
typedef struct { HvG7Check check[HV_G7_MAX_CHECKS]; int n; int blocked; } HvG7Report;

typedef struct {
    const char *dir;        /* the private directory that holds (or will hold) the pair */
    const char *ipsw_dir;   /* where HV_G7_IPSW_NAME lives */
    const char *ecid_dir;   /* where vm_ecid.txt lives (the Android app's per-VM file) */
    const char *rom_path;   /* the AVPBooter ROM file: a plain file of exactly HV_G7_ROM_BYTES with the known SHA-256 */
    uint64_t ecid;          /* the value the G7 request will use; must equal the file's. 0 = missing */
    uint64_t extra_free_bytes; /* added to the floor (a caller can only ask for MORE free space, never less) */
} HvG7Input;

/* Read-only. Returns 0 when nothing blocks, -1 otherwise. Needs a clean start: no disk, no AUX, no state file, no leftovers. */
int hv_g7_preflight(const HvG7Input *in, HvG7Report *rep);
void hv_g7_report_text(const HvG7Report *rep, char *out, size_t cap);

/* 0: a regular file (no link followed), HV_G7_ROM_BYTES long, SHA-256 equal to HV_G7_ROM_SHA256_HEX. -1 otherwise, with the reason in note. Read-only. */
int hv_g7_rom_check(const char *path, char *note, int cap);

typedef enum { HV_G7_ECID_MATCH = 0, HV_G7_ECID_MISSING, HV_G7_ECID_CORRUPT, HV_G7_ECID_MISMATCH, HV_G7_ECID_ERROR } HvG7Ecid;
HvG7Ecid hv_g7_ecid_check(const char *ecid_dir, uint64_t expected);

/* The only validator that accepts readonly_storage=0. Requires: installer=1, writable storage, ecid == expected != 0, seed == 0 (what the iOS installer uses), the
   disk and the AUX are exactly dir/macos_disk.img and dir/macos_aux.img (no blank_*, no other path), no host-diagnostic extras. Returns 0 or -1 with a reason. */
int hv_g7_config_ok(const HvVmConfig *c, const char *dir, uint64_t expected_ecid, char *why, size_t cap);

typedef enum { HV_G7_PAIR_NONE = 0, HV_G7_PAIR_IN_PROGRESS, HV_G7_PAIR_DONE, HV_G7_PAIR_INCONSISTENT, HV_G7_PAIR_ERROR } HvG7PairState;
HvG7PairState hv_g7_pair_state(const char *dir, char *note, int cap);
/* Creates the install disk exactly like the creator of host/disk_policy.h. The module does NOT call that function itself: creating the install disk is a
   separately approved step (tools/check-no-test-flags.sh forbids any app source from calling it), so the caller hands it in. Only the tests do so for now. */
typedef int (*HvG7DiskCreate)(const char *dir, char *note, int cap);
/* From NONE only: writes the state file (in_progress), creates the disk (with create_disk), the AUX and its initial copy. If a step fails, what THIS call
   created is removed again and the rest is untouched. Returns 0 or -1. */
int hv_g7_pair_begin(const char *dir, HvG7DiskCreate create_disk, char *note, int cap);
/* IN_PROGRESS -> DONE (atomic rename of a new state file). Only the caller that has CONFIRMED a complete restore may call it. */
int hv_g7_pair_finish(const char *dir, char *note, int cap);
/* The state right after hv_g7_pair_begin and BEFORE the machine is created: state is in_progress, the disk is the 128 GiB regular file, the AUX is a regular file of
   HV_G7_AUX_BYTES with the 24-byte header, and the AUX is byte-identical to its initial copy. The last point holds ONLY until a guest has run: once the restore has
   started the guest may legitimately change the AUX, so this must never be called after the machine has been started. Read-only. Returns 0 or -1 (note says why). */
int hv_g7_pair_verify_initial(const char *dir, char *note, int cap);
/* Moves the pair (disk, AUX, initial copy, state file) into g7_quarantine_<tag>/ by rename. Nothing is deleted or overwritten; an existing target is refused.
   tag: 1..32 of [a-z0-9_-]. Returns the number of items moved, or -1. */
int hv_g7_pair_quarantine(const char *dir, const char *tag, char *note, int cap);

#endif
