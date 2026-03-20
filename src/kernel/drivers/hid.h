#pragma once

#include <aevos/types.h>

/* ── input event types ── */

typedef enum {
    INPUT_KEY_PRESS       = 0,
    INPUT_KEY_RELEASE     = 1,
    INPUT_MOUSE_MOVE      = 2,
    INPUT_MOUSE_BUTTON    = 3,
    INPUT_MOUSE_SCROLL    = 4,
    INPUT_MOUSE_BTN_DOWN  = 5,
    INPUT_MOUSE_BTN_UP    = 6,
} input_event_type_t;

/* ── modifier flags ── */

#define MOD_LSHIFT  (1 << 0)
#define MOD_RSHIFT  (1 << 1)
#define MOD_LCTRL   (1 << 2)
#define MOD_RCTRL   (1 << 3)
#define MOD_LALT    (1 << 4)
#define MOD_RALT    (1 << 5)
#define MOD_CAPS    (1 << 6)
#define MOD_SHIFT   (MOD_LSHIFT | MOD_RSHIFT)
#define MOD_CTRL    (MOD_LCTRL  | MOD_RCTRL)
#define MOD_ALT     (MOD_LALT   | MOD_RALT)

/* ── mouse button flags ── */

#define MOUSE_BTN_LEFT   (1 << 0)
#define MOUSE_BTN_RIGHT  (1 << 1)
#define MOUSE_BTN_MIDDLE (1 << 2)

/* ── key codes ── */

typedef enum {
    KEY_NONE = 0,

    KEY_ESC = 1, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
    KEY_MINUS, KEY_EQUAL, KEY_BACKSPACE, KEY_TAB,
    KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P,
    KEY_LBRACKET, KEY_RBRACKET, KEY_ENTER, KEY_LCTRL,
    KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L,
    KEY_SEMICOLON, KEY_QUOTE, KEY_BACKTICK, KEY_LSHIFT,
    KEY_BACKSLASH, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M,
    KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_RSHIFT,
    KEY_KP_STAR, KEY_LALT, KEY_SPACE, KEY_CAPSLOCK,

    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10,

    KEY_NUMLOCK, KEY_SCROLLLOCK,
    KEY_KP_7, KEY_KP_8, KEY_KP_9, KEY_KP_MINUS,
    KEY_KP_4, KEY_KP_5, KEY_KP_6, KEY_KP_PLUS,
    KEY_KP_1, KEY_KP_2, KEY_KP_3, KEY_KP_0, KEY_KP_DOT,

    KEY_F11 = 87, KEY_F12 = 88,

    /* Extended keys (scan code set 1, 0xE0 prefix) */
    KEY_UP = 200, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_INSERT, KEY_DELETE,
    KEY_RCTRL, KEY_RALT,

    KEY_MAX
} keycode_t;

/* ── input event ── */

typedef struct input_event {
    input_event_type_t type;
    keycode_t          keycode;
    uint8_t            scancode;
    int32_t            mouse_x;
    int32_t            mouse_y;
    int32_t            mouse_dx;
    int32_t            mouse_dy;
    uint8_t            mouse_buttons;
    uint16_t           modifiers;
    uint8_t            button_changed;   /* which button triggered the event */
} input_event_t;

/* ── API ── */

void hid_init(void);
void hid_set_mouse_bounds(int32_t max_x, int32_t max_y);
void keyboard_handler(void);
void mouse_handler(void);
bool input_poll(input_event_t *ev);
bool input_wait(input_event_t *ev);
void input_get_mouse_pos(int32_t *x, int32_t *y);
uint8_t input_get_mouse_buttons(void);
keycode_t input_scancode_to_keycode(uint8_t scancode, bool extended);
void hid_poll_serial(void);
