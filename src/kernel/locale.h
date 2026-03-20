#pragma once

#include <aevos/types.h>
#include "drivers/hid.h"

/*
 * Locale / keyboard-layout support.
 * Converts keycodes + modifier state to printable characters.
 */

typedef enum {
    KB_LAYOUT_US = 0,
    KB_LAYOUT_COUNT
} kb_layout_t;

typedef struct {
    kb_layout_t layout;
    const char *name;
    const char *language;
    const char *encoding;
} locale_t;

void     locale_init(void);
void     locale_set_layout(kb_layout_t layout);
locale_t locale_get_current(void);

/*
 * Convert a keycode + modifier flags to a printable ASCII character.
 * Returns 0 if the keycode has no printable representation.
 */
char     keycode_to_char(keycode_t kc, uint16_t modifiers);
