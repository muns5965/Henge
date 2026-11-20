/* Henge, Android test mode only (compiled in only with -DHV_STOP_BEFORE_ROOTFS, which no shipped iOS build and no install path sets).
   "Stop A": idevicerestore_start returns right after it has chosen the build identity and checked that every component is in the IPSW, and decided whether the
   root filesystem would have to be extracted, but BEFORE it extracts anything, asks for a signature or touches the device. It reports what it found through
   hv_stop_a_hook and returns HV_STOP_BEFORE_ROOTFS_RC, a value no other exit of the function uses. */
#ifndef HV_STOP_H
#define HV_STOP_H
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
#endif
