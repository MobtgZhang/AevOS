#include "hid.h"
#include "../arch/io.h"
#include "../klog.h"

/* ── PS/2 controller ports ── */

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define PS2_STATUS_OUTPUT  0x01
#define PS2_STATUS_INPUT   0x02

/* ── event ring buffer ── */

#define EVENT_QUEUE_SIZE 256

static input_event_t event_queue[EVENT_QUEUE_SIZE];
static volatile uint32_t eq_head = 0;
static volatile uint32_t eq_tail = 0;

static uint16_t current_modifiers = 0;

/* ── mouse state ── */

static int32_t mouse_x = 0;
static int32_t mouse_y = 0;
static int32_t mouse_max_x = 1920;
static int32_t mouse_max_y = 1080;
static uint8_t mouse_buttons = 0;
static uint8_t mouse_cycle = 0;
static int8_t  mouse_bytes[3];

/* ── helpers ── */

static void push_event(const input_event_t *ev)
{
    uint32_t next = (eq_head + 1) % EVENT_QUEUE_SIZE;
    if (next == eq_tail) return;  /* full – drop oldest would be alternative */
    event_queue[eq_head] = *ev;
    eq_head = next;
}

static void ps2_wait_input(void)
{
    int timeout = 100000;
    while (timeout-- > 0) {
        if (!(inb(PS2_STATUS) & PS2_STATUS_INPUT)) return;
    }
}

static void ps2_wait_output(void)
{
    int timeout = 100000;
    while (timeout-- > 0) {
        if (inb(PS2_STATUS) & PS2_STATUS_OUTPUT) return;
    }
}

static void ps2_write_cmd(uint8_t cmd)
{
    ps2_wait_input();
    outb(PS2_CMD, cmd);
}

static void ps2_write_data(uint8_t data)
{
    ps2_wait_input();
    outb(PS2_DATA, data);
}

static uint8_t ps2_read_data(void)
{
    ps2_wait_output();
    return inb(PS2_DATA);
}

/*
 * ── Scan code set 1 → keycode translation ──
 *
 * Index = scancode (0x00–0x58).  Only make codes; break codes are
 * make | 0x80 and handled by stripping the high bit.
 */

static const keycode_t scancode_table[128] = {
    [0x00] = KEY_NONE,
    [0x01] = KEY_ESC,      [0x02] = KEY_1,         [0x03] = KEY_2,
    [0x04] = KEY_3,        [0x05] = KEY_4,         [0x06] = KEY_5,
    [0x07] = KEY_6,        [0x08] = KEY_7,         [0x09] = KEY_8,
    [0x0A] = KEY_9,        [0x0B] = KEY_0,         [0x0C] = KEY_MINUS,
    [0x0D] = KEY_EQUAL,    [0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
    [0x10] = KEY_Q,        [0x11] = KEY_W,         [0x12] = KEY_E,
    [0x13] = KEY_R,        [0x14] = KEY_T,         [0x15] = KEY_Y,
    [0x16] = KEY_U,        [0x17] = KEY_I,         [0x18] = KEY_O,
    [0x19] = KEY_P,        [0x1A] = KEY_LBRACKET,  [0x1B] = KEY_RBRACKET,
    [0x1C] = KEY_ENTER,    [0x1D] = KEY_LCTRL,
    [0x1E] = KEY_A,        [0x1F] = KEY_S,         [0x20] = KEY_D,
    [0x21] = KEY_F,        [0x22] = KEY_G,         [0x23] = KEY_H,
    [0x24] = KEY_J,        [0x25] = KEY_K,         [0x26] = KEY_L,
    [0x27] = KEY_SEMICOLON,[0x28] = KEY_QUOTE,     [0x29] = KEY_BACKTICK,
    [0x2A] = KEY_LSHIFT,   [0x2B] = KEY_BACKSLASH,
    [0x2C] = KEY_Z,        [0x2D] = KEY_X,         [0x2E] = KEY_C,
    [0x2F] = KEY_V,        [0x30] = KEY_B,         [0x31] = KEY_N,
    [0x32] = KEY_M,        [0x33] = KEY_COMMA,     [0x34] = KEY_DOT,
    [0x35] = KEY_SLASH,    [0x36] = KEY_RSHIFT,    [0x37] = KEY_KP_STAR,
    [0x38] = KEY_LALT,     [0x39] = KEY_SPACE,     [0x3A] = KEY_CAPSLOCK,
    [0x3B] = KEY_F1,       [0x3C] = KEY_F2,        [0x3D] = KEY_F3,
    [0x3E] = KEY_F4,       [0x3F] = KEY_F5,        [0x40] = KEY_F6,
    [0x41] = KEY_F7,       [0x42] = KEY_F8,        [0x43] = KEY_F9,
    [0x44] = KEY_F10,      [0x45] = KEY_NUMLOCK,   [0x46] = KEY_SCROLLLOCK,
    [0x47] = KEY_KP_7,     [0x48] = KEY_KP_8,      [0x49] = KEY_KP_9,
    [0x4A] = KEY_KP_MINUS, [0x4B] = KEY_KP_4,      [0x4C] = KEY_KP_5,
    [0x4D] = KEY_KP_6,     [0x4E] = KEY_KP_PLUS,   [0x4F] = KEY_KP_1,
    [0x50] = KEY_KP_2,     [0x51] = KEY_KP_3,      [0x52] = KEY_KP_0,
    [0x53] = KEY_KP_DOT,
    [0x57] = KEY_F11,      [0x58] = KEY_F12,
};

/* Extended scan codes (0xE0 prefix) */
static const keycode_t extended_table[128] = {
    [0x1C] = KEY_ENTER,    /* keypad enter */
    [0x1D] = KEY_RCTRL,
    [0x38] = KEY_RALT,
    [0x47] = KEY_HOME,
    [0x48] = KEY_UP,
    [0x49] = KEY_PGUP,
    [0x4B] = KEY_LEFT,
    [0x4D] = KEY_RIGHT,
    [0x4F] = KEY_END,
    [0x50] = KEY_DOWN,
    [0x51] = KEY_PGDN,
    [0x52] = KEY_INSERT,
    [0x53] = KEY_DELETE,
};

static void update_modifiers(keycode_t kc, bool pressed)
{
    uint16_t bit = 0;
    switch (kc) {
    case KEY_LSHIFT:   bit = MOD_LSHIFT; break;
    case KEY_RSHIFT:   bit = MOD_RSHIFT; break;
    case KEY_LCTRL:    bit = MOD_LCTRL;  break;
    case KEY_RCTRL:    bit = MOD_RCTRL;  break;
    case KEY_LALT:     bit = MOD_LALT;   break;
    case KEY_RALT:     bit = MOD_RALT;   break;
    case KEY_CAPSLOCK:
        if (pressed) current_modifiers ^= MOD_CAPS;
        return;
    default: return;
    }
    if (pressed) current_modifiers |= bit;
    else         current_modifiers &= ~bit;
}

/* ── keyboard IRQ handler (IRQ1) ── */

static bool     kb_extended = false;

void keyboard_handler(void)
{
    uint8_t sc = inb(PS2_DATA);

    if (sc == 0xE0) {
        kb_extended = true;
        return;
    }

    bool released = (sc & 0x80) != 0;
    uint8_t make = sc & 0x7F;

    keycode_t kc;
    if (kb_extended) {
        kc = (make < 128) ? extended_table[make] : KEY_NONE;
        kb_extended = false;
    } else {
        kc = (make < 128) ? scancode_table[make] : KEY_NONE;
    }

    if (kc == KEY_NONE) return;

    update_modifiers(kc, !released);

    input_event_t ev;
    ev.type           = released ? INPUT_KEY_RELEASE : INPUT_KEY_PRESS;
    ev.keycode        = kc;
    ev.scancode       = sc;
    ev.mouse_x        = mouse_x;
    ev.mouse_y        = mouse_y;
    ev.mouse_dx       = 0;
    ev.mouse_dy       = 0;
    ev.mouse_buttons  = mouse_buttons;
    ev.modifiers      = current_modifiers;
    ev.button_changed = 0;
    push_event(&ev);
}

/* ── mouse IRQ handler (IRQ12) ── */

void mouse_handler(void)
{
    uint8_t data = inb(PS2_DATA);

    switch (mouse_cycle) {
    case 0:
        mouse_bytes[0] = (int8_t)data;
        if (data & 0x08) mouse_cycle++;  /* bit 3 always set in first byte */
        break;
    case 1:
        mouse_bytes[1] = (int8_t)data;
        mouse_cycle++;
        break;
    case 2: {
        mouse_bytes[2] = (int8_t)data;
        mouse_cycle = 0;

        int32_t dx = mouse_bytes[1];
        int32_t dy = -mouse_bytes[2];  /* PS/2 Y is inverted */

        mouse_x += dx;
        mouse_y += dy;
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_x >= mouse_max_x) mouse_x = mouse_max_x - 1;
        if (mouse_y >= mouse_max_y) mouse_y = mouse_max_y - 1;

        uint8_t new_buttons = (uint8_t)(mouse_bytes[0] & 0x07);
        uint8_t old_buttons = mouse_buttons;
        mouse_buttons = new_buttons;

        /* Generate movement event */
        if (dx || dy) {
            input_event_t ev;
            ev.type           = INPUT_MOUSE_MOVE;
            ev.keycode        = KEY_NONE;
            ev.scancode       = 0;
            ev.mouse_x        = mouse_x;
            ev.mouse_y        = mouse_y;
            ev.mouse_dx       = dx;
            ev.mouse_dy       = dy;
            ev.mouse_buttons  = new_buttons;
            ev.modifiers      = current_modifiers;
            ev.button_changed = 0;
            push_event(&ev);
        }

        /* Generate per-button press/release events */
        uint8_t changed = old_buttons ^ new_buttons;
        if (changed) {
            for (uint8_t bit = 0; bit < 3; bit++) {
                uint8_t mask = (uint8_t)(1u << bit);
                if (!(changed & mask)) continue;

                input_event_t ev;
                ev.type           = (new_buttons & mask)
                                    ? INPUT_MOUSE_BTN_DOWN
                                    : INPUT_MOUSE_BTN_UP;
                ev.keycode        = KEY_NONE;
                ev.scancode       = 0;
                ev.mouse_x        = mouse_x;
                ev.mouse_y        = mouse_y;
                ev.mouse_dx       = 0;
                ev.mouse_dy       = 0;
                ev.mouse_buttons  = new_buttons;
                ev.modifiers      = current_modifiers;
                ev.button_changed = mask;
                push_event(&ev);
            }
        }

        /* Legacy combined event for backward compatibility */
        if (!dx && !dy && !changed) {
            input_event_t ev;
            ev.type           = INPUT_MOUSE_BUTTON;
            ev.keycode        = KEY_NONE;
            ev.scancode       = 0;
            ev.mouse_x        = mouse_x;
            ev.mouse_y        = mouse_y;
            ev.mouse_dx       = 0;
            ev.mouse_dy       = 0;
            ev.mouse_buttons  = new_buttons;
            ev.modifiers      = current_modifiers;
            ev.button_changed = 0;
            push_event(&ev);
        }
        break;
    }
    }
}

/* ── initialization ── */

static void mouse_init_ps2(void)
{
    /* Enable auxiliary device (mouse) */
    ps2_write_cmd(0xA8);

    /* Enable interrupts on controller */
    ps2_write_cmd(0x20);
    uint8_t status = ps2_read_data();
    status |= 0x02;  /* enable IRQ12 */
    status &= ~0x20; /* enable mouse clock */
    ps2_write_cmd(0x60);
    ps2_write_data(status);

    /* Tell mouse to use default settings */
    ps2_write_cmd(0xD4);
    ps2_write_data(0xF6);
    ps2_read_data();  /* ACK */

    /* Enable data reporting */
    ps2_write_cmd(0xD4);
    ps2_write_data(0xF4);
    ps2_read_data();  /* ACK */

    mouse_cycle = 0;
    mouse_x = 0;
    mouse_y = 0;
    mouse_buttons = 0;
}

void hid_init(void)
{
    eq_head = 0;
    eq_tail = 0;
    current_modifiers = 0;
    kb_extended = false;

    /* Flush the PS/2 buffer */
    while (inb(PS2_STATUS) & PS2_STATUS_OUTPUT)
        inb(PS2_DATA);

    mouse_init_ps2();

    klog("[hid] PS/2 keyboard + mouse initialized\n");
}

/* ── event polling ── */

bool input_poll(input_event_t *ev)
{
    if (eq_tail == eq_head) return false;
    *ev = event_queue[eq_tail];
    eq_tail = (eq_tail + 1) % EVENT_QUEUE_SIZE;
    return true;
}

bool input_wait(input_event_t *ev)
{
    while (eq_tail == eq_head) {
#if defined(__x86_64__)
        __asm__ volatile("hlt");
#elif defined(__aarch64__)
        __asm__ volatile("wfi");
#elif defined(__riscv)
        __asm__ volatile("wfi");
#elif defined(__loongarch64)
        __asm__ volatile("idle 0");
#endif
    }
    *ev = event_queue[eq_tail];
    eq_tail = (eq_tail + 1) % EVENT_QUEUE_SIZE;
    return true;
}

void hid_set_mouse_bounds(int32_t max_x, int32_t max_y)
{
    mouse_max_x = max_x;
    mouse_max_y = max_y;
}

void input_get_mouse_pos(int32_t *x, int32_t *y)
{
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
}

uint8_t input_get_mouse_buttons(void)
{
    return mouse_buttons;
}

keycode_t input_scancode_to_keycode(uint8_t scancode, bool extended)
{
    uint8_t make = scancode & 0x7F;
    if (make >= 128) return KEY_NONE;
    return extended ? extended_table[make] : scancode_table[make];
}
