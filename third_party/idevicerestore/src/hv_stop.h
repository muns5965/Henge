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
#endif
