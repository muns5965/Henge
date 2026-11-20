/* Henge, Android test mode only (compiled in only with -DHV_STOP_BEFORE_ROOTFS, which no shipped iOS build and no install path sets).
   "Stop A": idevicerestore_start returns right after it has chosen the build identity and checked that every component is in the IPSW, and decided whether the
   root filesystem would have to be extracted, but BEFORE it extracts anything, asks for a signature or touches the device. It reports what it found through
   hv_stop_a_hook and returns HV_STOP_BEFORE_ROOTFS_RC, a value no other exit of the function uses. */
#ifndef HV_STOP_H
#define HV_STOP_H
#include <stdint.h>
#ifdef HV_STOP_BEFORE_ROOTFS
#include <plist/plist.h>
#define FLAG_STOP_BEFORE_ROOTFS (1 << 20)
#define HV_STOP_BEFORE_ROOTFS_RC 217
struct hv_stop_a_info {
	plist_t build_identity;      /* the chosen one (borrowed; valid during the call) */
	const char *hardware_model;
	const char *product_type;
	const char *os_path;         /* the root filesystem entry inside the IPSW */
	const char *ipsw_path;
	const char *cache_dir;       /* where an extraction would go (may be NULL: then a temp file) */
	int zip;                     /* the IPSW is a ZIP, not a directory */
	int os_opened;               /* ipsw_file_open on the OS entry succeeded */
	int seekable;                /* ... and the handle is seekable (the entry is stored) */
	int needs_os_extraction;
	int shsh_only;
	int macos_variant;           /* a macOS recovery-OS variant exists for this model */
	int tss_enabled;
};
extern void (*hv_stop_a_hook)(const struct hv_stop_a_info *info);
#endif

/* Henge, Android test mode only (compiled in only with -DHV_STOP_AFTER_DFU_RECOVERY, which no shipped iOS build and no install path sets).
   "Stop B": idevicerestore_start returns right after the device has left DFU (or Recovery+iBEC) mode and reconnected in Recovery mode — i.e. right after the
   personalized firmware (iBSS/iBEC, already signed by the TSS tickets fetched earlier in this same call) has been sent and the device has disconnected and
   reconnected — but BEFORE the post-mode-change AP-nonce recheck/re-signing step and BEFORE recovery_enter_restore()/restore_device() (the actual disk-writing
   restore). Unlike stop A, this point is reached only after real signing tickets were used and real firmware WAS transferred to the device; it reports what it
   found through hv_stop_b_hook and returns HV_STOP_AFTER_DFU_RECOVERY_RC, a value no other exit of the function uses. */
#ifdef HV_STOP_AFTER_DFU_RECOVERY
#define FLAG_STOP_AFTER_DFU_RECOVERY (1 << 21)
#define HV_STOP_AFTER_DFU_RECOVERY_RC 218
struct hv_stop_b_info {
	const char *hardware_model;
	const char *product_type;
	int mode_before;      /* client->mode's index into idevicerestore_modes[] right before this step started (2 = DFU, 3 = RECOVERY) */
	int mode_after;       /* same, now; expected 3 (RECOVERY) on success */
	int tss_enabled;
	int macos_variant;    /* a macOS recovery-OS variant exists for this model */
};
extern void (*hv_stop_b_hook)(const struct hv_stop_b_info *info);
#endif

/* Henge, Android test mode only (compiled in only with -DHV_STOP_AFTER_RECOVERYOS_ENTRY, which no shipped iOS build and no install path sets).
   "Stop C": idevicerestore_start returns right after the device has been confirmed in MODE_RESTORE -- i.e. right after recovery_enter_restore() has sent
   AppleLogo, the components iBoot loads, the restore ramdisk, the device tree, (SEP firmware, if present) and the kernelcache, the device disconnected from
   Recovery and reconnected running that ramdisk (RecoveryOS), and idevicerestore's own wait for MODE_RESTORE succeeded -- but BEFORE "About to restore
   device..." and restore_device() (the actual ASR/FDR disk-writing restore). Like stop B, every component sent up to this point was already signed by the
   TSS tickets fetched earlier in this same call and extracted from the already-downloaded IPSW: this step asks for no new signature and reaches no new
   network host. It reports what it found through hv_stop_c_hook and returns HV_STOP_AFTER_RECOVERYOS_ENTRY_RC, a value no other exit of the function uses. */
#ifdef HV_STOP_AFTER_RECOVERYOS_ENTRY
#define FLAG_STOP_AFTER_RECOVERYOS_ENTRY (1 << 22)
#define HV_STOP_AFTER_RECOVERYOS_ENTRY_RC 219
struct hv_stop_c_info {
	const char *hardware_model;
	const char *product_type;
	int mode_before;      /* client->mode's index into idevicerestore_modes[] right before this step started (3 = RECOVERY) */
	int mode_after;       /* same, now; expected 4 (RESTORE) on success */
	int tss_enabled;
	int macos_variant;    /* a macOS recovery-OS variant exists for this model */
};
extern void (*hv_stop_c_hook)(const struct hv_stop_c_info *info);
#endif

/* Henge, Android test mode only (compiled in only with -DHV_STOP_AFTER_RESTORED_HANDSHAKE, which no shipped iOS build and no install path sets).
   "Stop D": restore_device() returns right after it has opened the device's `restored` service (restore_open_with_timeout: confirms the response type is
   "com.apple.mobile.restored" and records its protocol version -- reaching this stop point is itself proof that succeeded, since restore_device() returns
   early otherwise), queried HardwareInfo and SavedDebugInfo (purely informational queries; restore_device() tolerates either failing without aborting), and
   done the conditional, no-new-request BBTicket copy -- but BEFORE FDR starts and BEFORE restored_start_restore() (the actual disk-writing restore command).
   Unlike stops A/B/C (which all return from idevicerestore_start itself, before restore_device() is ever called), stop D is the first point reached INSIDE
   restore_device() -- the first point where Henge's test harness talks to a real restored session at all. Every byte exchanged to reach this point was
   already signed by the TSS tickets fetched earlier in this same call, and the host never sends the actual restore-start command, so storage writes continue
   to be refused exactly as before this point (the guest's own AUX write attempts, observed since Henge build 155, are unaffected and remain refused -- this is
   not "no disk-related signal is sent at all", only "the host never sends the restore-start command"). It reports what it found through hv_stop_d_hook and
   returns HV_STOP_AFTER_RESTORED_HANDSHAKE_RC from restore_device(), a value no other normal exit of that function uses. */
#ifdef HV_STOP_AFTER_RESTORED_HANDSHAKE
#define FLAG_STOP_AFTER_RESTORED_HANDSHAKE (1 << 23)
#define HV_STOP_AFTER_RESTORED_HANDSHAKE_RC 220
/* HardwareInfo is never legitimately "not provided" (restore_device() always queries it and a real restored session always has hardware info to give), so
   it is tracked as OK/FAILED only. SavedDebugInfo legitimately has nothing to give on a device with no prior restore attempt, so it gets the third state. */
enum { HV_INFO_NOT_PROVIDED = 0, HV_INFO_OK = 1, HV_INFO_FAILED = 2 };
struct hv_stop_d_info {
	const char *hardware_model;
	const char *product_type;
	int mode;                /* client->mode's index into idevicerestore_modes[] at this point (4 = RESTORE expected; restore_device() is only ever called
	                             while client->mode == MODE_RESTORE, and nothing before stop D changes it, but this makes that a checkable fact rather than
	                             an assumption about the source) */
	uint64_t restored_protocol_version; /* client->restore->protocol_version, set by restore_open_with_timeout() once it confirmed the service type */
	int hwinfo_status;       /* HV_INFO_OK or HV_INFO_FAILED: restored_query_value(restore, "HardwareInfo", ...) == RESTORE_E_SUCCESS or not */
	int saved_debug_status;  /* HV_INFO_NOT_PROVIDED, HV_INFO_OK or HV_INFO_FAILED, from restored_query_value(restore, "SavedDebugInfo", ...): SUCCESS -> OK;
	                             the device validly answered but had no SavedDebugInfo entry (RESTORE_E_PLIST_ERROR, from restored's own
	                             QueryValue handling of a missing key) -> NOT_PROVIDED; anything else (a transport/communication error) -> FAILED */
	int bbticket_present;     /* 1 if client->tss had a BBTicket entry (and client->restore->bbtss now holds a copy); 0 for a device/build with no baseband
	                              (this project's virtual Mac, for instance) -- NOT required for stop D to count as reached */
	int tss_enabled;
};
extern void (*hv_stop_d_hook)(const struct hv_stop_d_info *info);
#endif

/* Henge, Android test mode only (compiled in only with -DHV_STOP_AFTER_RESTORE_OPTIONS, which no shipped iOS build and no install path sets).
   "Stop E": restore_device() returns right after the restore-options plist is fully built (the last field it sets is "SystemPartitionPadding") and right
   after it has tried to start the Reverse Proxy (this build always defines HAVE_REVERSE_PROXY -- third_party/restore_config/config.h -- so this is the
   branch that actually compiles here; the FDR-listener-thread branch does not) -- but strictly BEFORE restored_start_restore() (the actual disk-writing
   restore command) is ever called. docs/g6-stop-e-design-2026-11-20.md is the full design; this comment only gives the return-code contract.

   Two distinct returns, because the Reverse Proxy's own cleanup (reverse_proxy_client_free()) can leave a genuinely unresolved case: it returns
   REVERSE_PROXY_E_UNKNOWN_ERROR both when its internal thread_join() failed (nothing freed, the thread may still reference client->restore->device) and
   when the join succeeded but a later, unrelated step failed (everything already freed) -- the two are NOT distinguishable from that return value alone.
   - HV_STOP_AFTER_RESTORE_OPTIONS_RC: the normal stop. Either there was nothing to join (no Reverse Proxy thread ever started, or free() returned anything
     other than UNKNOWN_ERROR, which only happens once the join has already succeeded), and restore_client_free(client) ran normally before returning.
   - HV_STOP_E_CLEANUP_UNCERTAIN_RC: free() returned exactly UNKNOWN_ERROR. client->restore (and the idevice_t it shares with the Reverse Proxy client) is
     deliberately NOT freed in this case -- only opts is freed, which has nothing to do with the Reverse Proxy and is always safe to free. A G6 diagnostic
     build must treat this return as a failure, never as any kind of pass, and must not call this function again on the same client. */
#ifdef HV_STOP_AFTER_RESTORE_OPTIONS
#define FLAG_STOP_AFTER_RESTORE_OPTIONS (1 << 24)
#define HV_STOP_AFTER_RESTORE_OPTIONS_RC 221
#define HV_STOP_E_CLEANUP_UNCERTAIN_RC 222
enum { HV_RP_FAILED = 0, HV_RP_OK = 1 };   /* rproxy_status: whether reverse_proxy_client_start_proxy() actually returned SUCCESS (rp_started), not whether the pointer is non-NULL */
struct hv_stop_e_info {
    const char *hardware_model;
    const char *product_type;
    int mode;                         /* client->mode's index into idevicerestore_modes[] at this point (4 = RESTORE expected, same check as stop D) */
    uint64_t restored_protocol_version;
    int rproxy_status;                /* HV_RP_OK or HV_RP_FAILED, from rp_started -- see restore.c */
    int opts_required_fields_ok;      /* the fixed required-field/type check on the options plist (docs/g6-stop-e-design-2026-11-20.md §8) */
    int tss_enabled;
};
extern void (*hv_stop_e_hook)(const struct hv_stop_e_info *info);
#endif
#endif
