#pragma once

#include <aevos/types.h>
#include "widget.h"

typedef struct fb_ctx fb_ctx_t;
typedef struct input_event input_event_t;
typedef struct agent agent_t;

#define SIDEBAR_MAX_ITEMS  256

typedef enum {
    SIDEBAR_ITEM_FILE,
    SIDEBAR_ITEM_DIR,
    SIDEBAR_ITEM_AGENT,
    SIDEBAR_ITEM_SKILL,
    SIDEBAR_ITEM_MEMORY
} sidebar_item_type_t;

typedef enum {
    SIDEBAR_SECTION_FILES,
    SIDEBAR_SECTION_AGENTS,
    SIDEBAR_SECTION_SKILLS,
    SIDEBAR_SECTION_MEMORY,
    SIDEBAR_SECTION_COUNT
} sidebar_section_t;

typedef struct {
    char                name[64];
    sidebar_item_type_t type;
    bool                is_expanded;
    int                 indent_level;
    char                icon_char;
    bool                is_section_header;
} sidebar_item_t;

typedef struct {
    sidebar_item_t items[SIDEBAR_MAX_ITEMS];
    int            item_count;
    int            max_items;
    int            selected_index;
    int            scroll_offset;
    rect_t         bounds;
    bool           sections_expanded[SIDEBAR_SECTION_COUNT];
} sidebar_t;

void sidebar_init(sidebar_t *sb, rect_t bounds);
void sidebar_populate(sidebar_t *sb, agent_t *agent);
void sidebar_render(sidebar_t *sb, fb_ctx_t *fb);
bool sidebar_handle_input(sidebar_t *sb, input_event_t *ev);
