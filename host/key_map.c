#include "key_map.h"

#include <stddef.h>

/* Linux KEY_* values (linux/input-event-codes.h) */
enum {
    L_ESC = 1, L_1 = 2, L_2, L_3, L_4, L_5, L_6, L_7, L_8, L_9, L_0, L_MINUS = 12, L_EQUAL = 13, L_BACKSPACE = 14, L_TAB = 15,
    L_Q = 16, L_W, L_E, L_R, L_T, L_Y, L_U, L_I, L_O, L_P, L_LEFTBRACE = 26, L_RIGHTBRACE = 27, L_ENTER = 28, L_LEFTCTRL = 29,
    L_A = 30, L_S, L_D, L_F, L_G, L_H, L_J, L_K, L_L, L_SEMICOLON = 39, L_APOSTROPHE = 40, L_GRAVE = 41, L_LEFTSHIFT = 42, L_BACKSLASH = 43,
    L_Z = 44, L_X, L_C, L_V, L_B, L_N, L_M, L_COMMA = 51, L_DOT = 52, L_SLASH = 53, L_RIGHTSHIFT = 54, L_LEFTALT = 56, L_SPACE = 57, L_CAPSLOCK = 58,
    L_F1 = 59, L_F2, L_F3, L_F4, L_F5, L_F6, L_F7, L_F8, L_F9, L_F10 = 68, L_NUMLOCK = 69, L_SCROLLLOCK = 70, L_F11 = 87, L_F12 = 88,
    L_RIGHTCTRL = 97, L_RIGHTALT = 100, L_HOME = 102, L_UP = 103, L_PAGEUP = 104, L_LEFT = 105, L_RIGHT = 106, L_END = 107, L_DOWN = 108, L_PAGEDOWN = 109,
    L_INSERT = 110, L_DELETE = 111, L_LEFTMETA = 125, L_RIGHTMETA = 126
};

static const struct { int a; uint16_t l; } map[] = {
#define KEYMAP(name, num, lin) {num, lin},
#include "key_map_table.h"
#undef KEYMAP
};

uint16_t hv_keymap_android(int code) {
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (map[i].a == code) return map[i].l;
    return 0;
}

int hv_keymap_entry(int i, int *a, uint16_t *l) {
    if (i < 0 || (size_t)i >= sizeof map / sizeof map[0]) return -1;
    if (a) *a = map[i].a;
    if (l) *l = map[i].l;
    return 0;
}
