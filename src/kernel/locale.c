#include "locale.h"
#include "klog.h"

static locale_t current_locale;

/*
 * US keyboard layout: keycode → ASCII (unshifted / shifted).
 * Index = keycode_t value (only KEY_1 .. KEY_SLASH + extras).
 */

typedef struct {
    char normal;
    char shifted;
} key_mapping_t;

#define KC_MAP_SIZE (KEY_MAX + 1)
static key_mapping_t us_layout[KC_MAP_SIZE];

static void init_us_layout(void)
{
    for (int i = 0; i < KC_MAP_SIZE; i++) {
        us_layout[i].normal  = 0;
        us_layout[i].shifted = 0;
    }

    /* Row 1: number keys */
    us_layout[KEY_1] = (key_mapping_t){ '1', '!' };
    us_layout[KEY_2] = (key_mapping_t){ '2', '@' };
    us_layout[KEY_3] = (key_mapping_t){ '3', '#' };
    us_layout[KEY_4] = (key_mapping_t){ '4', '$' };
    us_layout[KEY_5] = (key_mapping_t){ '5', '%' };
    us_layout[KEY_6] = (key_mapping_t){ '6', '^' };
    us_layout[KEY_7] = (key_mapping_t){ '7', '&' };
    us_layout[KEY_8] = (key_mapping_t){ '8', '*' };
    us_layout[KEY_9] = (key_mapping_t){ '9', '(' };
    us_layout[KEY_0] = (key_mapping_t){ '0', ')' };

    us_layout[KEY_MINUS]     = (key_mapping_t){ '-', '_' };
    us_layout[KEY_EQUAL]     = (key_mapping_t){ '=', '+' };
    us_layout[KEY_BACKTICK]  = (key_mapping_t){ '`', '~' };

    /* Row 2: QWERTY */
    us_layout[KEY_Q] = (key_mapping_t){ 'q', 'Q' };
    us_layout[KEY_W] = (key_mapping_t){ 'w', 'W' };
    us_layout[KEY_E] = (key_mapping_t){ 'e', 'E' };
    us_layout[KEY_R] = (key_mapping_t){ 'r', 'R' };
    us_layout[KEY_T] = (key_mapping_t){ 't', 'T' };
    us_layout[KEY_Y] = (key_mapping_t){ 'y', 'Y' };
    us_layout[KEY_U] = (key_mapping_t){ 'u', 'U' };
    us_layout[KEY_I] = (key_mapping_t){ 'i', 'I' };
    us_layout[KEY_O] = (key_mapping_t){ 'o', 'O' };
    us_layout[KEY_P] = (key_mapping_t){ 'p', 'P' };

    us_layout[KEY_LBRACKET]  = (key_mapping_t){ '[', '{' };
    us_layout[KEY_RBRACKET]  = (key_mapping_t){ ']', '}' };
    us_layout[KEY_BACKSLASH] = (key_mapping_t){ '\\', '|' };

    /* Row 3: ASDF */
    us_layout[KEY_A] = (key_mapping_t){ 'a', 'A' };
    us_layout[KEY_S] = (key_mapping_t){ 's', 'S' };
    us_layout[KEY_D] = (key_mapping_t){ 'd', 'D' };
    us_layout[KEY_F] = (key_mapping_t){ 'f', 'F' };
    us_layout[KEY_G] = (key_mapping_t){ 'g', 'G' };
    us_layout[KEY_H] = (key_mapping_t){ 'h', 'H' };
    us_layout[KEY_J] = (key_mapping_t){ 'j', 'J' };
    us_layout[KEY_K] = (key_mapping_t){ 'k', 'K' };
    us_layout[KEY_L] = (key_mapping_t){ 'l', 'L' };

    us_layout[KEY_SEMICOLON] = (key_mapping_t){ ';', ':' };
    us_layout[KEY_QUOTE]     = (key_mapping_t){ '\'', '"' };

    /* Row 4: ZXCV */
    us_layout[KEY_Z] = (key_mapping_t){ 'z', 'Z' };
    us_layout[KEY_X] = (key_mapping_t){ 'x', 'X' };
    us_layout[KEY_C] = (key_mapping_t){ 'c', 'C' };
    us_layout[KEY_V] = (key_mapping_t){ 'v', 'V' };
    us_layout[KEY_B] = (key_mapping_t){ 'b', 'B' };
    us_layout[KEY_N] = (key_mapping_t){ 'n', 'N' };
    us_layout[KEY_M] = (key_mapping_t){ 'm', 'M' };

    us_layout[KEY_COMMA] = (key_mapping_t){ ',', '<' };
    us_layout[KEY_DOT]   = (key_mapping_t){ '.', '>' };
    us_layout[KEY_SLASH] = (key_mapping_t){ '/', '?' };

    /* Whitespace / special */
    us_layout[KEY_SPACE] = (key_mapping_t){ ' ', ' ' };
    us_layout[KEY_TAB]   = (key_mapping_t){ '\t', '\t' };

    /* Keypad digits */
    us_layout[KEY_KP_0]     = (key_mapping_t){ '0', '0' };
    us_layout[KEY_KP_1]     = (key_mapping_t){ '1', '1' };
    us_layout[KEY_KP_2]     = (key_mapping_t){ '2', '2' };
    us_layout[KEY_KP_3]     = (key_mapping_t){ '3', '3' };
    us_layout[KEY_KP_4]     = (key_mapping_t){ '4', '4' };
    us_layout[KEY_KP_5]     = (key_mapping_t){ '5', '5' };
    us_layout[KEY_KP_6]     = (key_mapping_t){ '6', '6' };
    us_layout[KEY_KP_7]     = (key_mapping_t){ '7', '7' };
    us_layout[KEY_KP_8]     = (key_mapping_t){ '8', '8' };
    us_layout[KEY_KP_9]     = (key_mapping_t){ '9', '9' };
    us_layout[KEY_KP_DOT]   = (key_mapping_t){ '.', '.' };
    us_layout[KEY_KP_STAR]  = (key_mapping_t){ '*', '*' };
    us_layout[KEY_KP_MINUS] = (key_mapping_t){ '-', '-' };
    us_layout[KEY_KP_PLUS]  = (key_mapping_t){ '+', '+' };
}

void locale_init(void)
{
    current_locale.layout   = KB_LAYOUT_US;
    current_locale.name     = "en_US";
    current_locale.language = "English";
    current_locale.encoding = "ASCII";

    init_us_layout();

    klog("[locale] keyboard layout: US English\n");
}

void locale_set_layout(kb_layout_t layout)
{
    if (layout >= KB_LAYOUT_COUNT) return;
    current_locale.layout = layout;

    switch (layout) {
    case KB_LAYOUT_US:
    default:
        init_us_layout();
        current_locale.name     = "en_US";
        current_locale.language = "English";
        break;
    }
}

locale_t locale_get_current(void)
{
    return current_locale;
}

char keycode_to_char(keycode_t kc, uint16_t modifiers)
{
    if (kc >= KC_MAP_SIZE) return 0;

    bool shift = (modifiers & MOD_SHIFT) != 0;
    bool caps  = (modifiers & MOD_CAPS) != 0;

    key_mapping_t m = us_layout[kc];
    if (m.normal == 0) return 0;

    char c = shift ? m.shifted : m.normal;

    /* CapsLock only affects letters */
    if (caps && c >= 'a' && c <= 'z')
        c -= 32;
    else if (caps && !shift && c >= 'A' && c <= 'Z')
        c += 32;

    return c;
}
