/* Compile-time check of host/key_map_table.h against the NDK's own key codes: a wrong number in the table fails the build instead of typing the wrong key.
   (The Linux side of the table is checked by tests/test_c0b0.c.) */
#include <android/keycodes.h>

#define KEYMAP(name, num, lin) _Static_assert((name) == (num), #name " does not have the number written in host/key_map_table.h");
#include "key_map_table.h"
#undef KEYMAP

int henge_keycheck_present(void) { return 1; }
