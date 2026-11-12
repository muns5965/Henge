#ifndef HENGEVM_KEY_MAP_H
#define HENGEVM_KEY_MAP_H
/* C0: Android key codes (android.view.KeyEvent.KEYCODE_*, the numbers of <android/keycodes.h>) to the Linux input codes hv_vm_key takes (KEY_A = 30, as in linux/input-event-codes.h).
   Pure table, no Android headers: the numbers are written out here and the Android build checks every one of them against the NDK's own header at compile time
   (android/app/src/main/cpp/keycheck.c), so a wrong number breaks the build instead of typing the wrong key.
   Only keys a Mac keyboard has and a phone's hardware or on-screen keyboard can send are mapped. Text that comes from an input method (composed Korean, emoji, anything that is not a key)
   is NOT handled here: it has no key code. */
#include <stdint.h>

/* The Linux code for an Android key code, or 0 when the key is not mapped. */
uint16_t hv_keymap_android(int android_keycode);

/* The table, for tests and for the compile-time check: entry i (0-based) as {android, linux}; returns 0, or -1 past the end. */
int hv_keymap_entry(int i, int *android_keycode, uint16_t *linux_code);

#endif
