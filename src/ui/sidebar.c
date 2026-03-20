#include "sidebar.h"
#include "theme.h"
#include "font.h"
#include "../kernel/drivers/gpu_fb.h"
#include "../kernel/drivers/hid.h"
#include "../kernel/klog.h"
#include "../agent/agent_core.h"
#include "../lib/string.h"

void sidebar_init(sidebar_t *sb, rect_t bounds)
{
    if (!sb)
        return;

    memset(sb, 0, sizeof(sidebar_t));
    sb->bounds        = bounds;
    sb->max_items     = SIDEBAR_MAX_ITEMS;
    sb->item_count    = 0;
    sb->selected_index = -1;
    sb->scroll_offset = 0;

    for (int i = 0; i < SIDEBAR_SECTION_COUNT; i++)
        sb->sections_expanded[i] = true;
}

static void sidebar_add_item(sidebar_t *sb, const char *name,
                             sidebar_item_type_t type, int indent,
                             char icon, bool is_header)
{
    if (sb->item_count >= sb->max_items)
        return;

    sidebar_item_t *item = &sb->items[sb->item_count];
    strncpy(item->name, name, 63);
    item->name[63]          = '\0';
    item->type              = type;
    item->indent_level      = indent;
    item->icon_char         = icon;
    item->is_expanded       = false;
    item->is_section_header = is_header;
    sb->item_count++;
}

void sidebar_populate(sidebar_t *sb, agent_t *agent)
{
    if (!sb)
        return;

    sb->item_count = 0;

    sidebar_add_item(sb, "[F] Files", SIDEBAR_ITEM_DIR, 0, 'F', true);
    if (sb->sections_expanded[SIDEBAR_SECTION_FILES]) {
        sidebar_add_item(sb, "kernel/", SIDEBAR_ITEM_DIR, 1, '>', false);
        sidebar_add_item(sb, "agent/", SIDEBAR_ITEM_DIR, 1, '>', false);
        sidebar_add_item(sb, "ui/", SIDEBAR_ITEM_DIR, 1, '>', false);
        sidebar_add_item(sb, "lib/", SIDEBAR_ITEM_DIR, 1, '>', false);
        sidebar_add_item(sb, "llm/", SIDEBAR_ITEM_DIR, 1, '>', false);
        sidebar_add_item(sb, "boot/", SIDEBAR_ITEM_DIR, 1, '>', false);
    }

    sidebar_add_item(sb, "[A] Agents", SIDEBAR_ITEM_AGENT, 0, 'A', true);
    if (sb->sections_expanded[SIDEBAR_SECTION_AGENTS]) {
        if (agent && agent->name[0]) {
            sidebar_add_item(sb, agent->name, SIDEBAR_ITEM_AGENT,
                             1, '*', false);
        } else {
            sidebar_add_item(sb, "default", SIDEBAR_ITEM_AGENT,
                             1, '*', false);
        }
    }

    sidebar_add_item(sb, "[S] Skills", SIDEBAR_ITEM_SKILL, 0, 'S', true);
    if (sb->sections_expanded[SIDEBAR_SECTION_SKILLS]) {
        if (agent && agent->skills.count > 0) {
            sidebar_add_item(sb, "code_gen", SIDEBAR_ITEM_SKILL,
                             1, '#', false);
            sidebar_add_item(sb, "summarize", SIDEBAR_ITEM_SKILL,
                             1, '#', false);
            sidebar_add_item(sb, "analyze", SIDEBAR_ITEM_SKILL,
                             1, '#', false);
        } else {
            sidebar_add_item(sb, "(none loaded)", SIDEBAR_ITEM_SKILL,
                             1, '-', false);
        }
    }

    sidebar_add_item(sb, "[M] Memory", SIDEBAR_ITEM_MEMORY, 0, 'M', true);
    if (sb->sections_expanded[SIDEBAR_SECTION_MEMORY]) {
        sidebar_add_item(sb, "episodic", SIDEBAR_ITEM_MEMORY,
                         1, 'e', false);
        sidebar_add_item(sb, "semantic", SIDEBAR_ITEM_MEMORY,
                         1, 's', false);
        sidebar_add_item(sb, "procedural", SIDEBAR_ITEM_MEMORY,
                         1, 'p', false);
    }
}

void sidebar_render(sidebar_t *sb, fb_ctx_t *fb)
{
    if (!sb || !fb)
        return;

    fb_draw_rect(sb->bounds.x, sb->bounds.y,
                 sb->bounds.w, sb->bounds.h, COLOR_SIDEBAR);

    fb_draw_rect(sb->bounds.x + sb->bounds.w - 1, sb->bounds.y,
                 1, sb->bounds.h, COLOR_DIVIDER);

    const font_t *fnt = font_get_default();
    int item_h = fnt->height + MARGIN * 2;
    int y = sb->bounds.y + PADDING - sb->scroll_offset;

    for (int i = 0; i < sb->item_count; i++) {
        sidebar_item_t *item = &sb->items[i];

        if (y + item_h < sb->bounds.y || y > sb->bounds.y + sb->bounds.h) {
            y += item_h;
            continue;
        }

        if (i == sb->selected_index) {
            fb_draw_rect(sb->bounds.x, y,
                         sb->bounds.w - 1, item_h,
                         color_brighten(COLOR_SIDEBAR, 20));
            fb_draw_rect(sb->bounds.x, y, 3, item_h, COLOR_ACCENT);
        }

        int text_x = sb->bounds.x + PADDING +
                      item->indent_level * (fnt->width * 2);
        int text_y = y + MARGIN;

        uint32_t text_color;
        if (item->is_section_header)
            text_color = COLOR_TEXT;
        else if (i == sb->selected_index)
            text_color = COLOR_TEXT;
        else
            text_color = COLOR_TEXT_DIM;

        uint32_t icon_color;
        switch (item->type) {
        case SIDEBAR_ITEM_DIR:   icon_color = COLOR_YELLOW; break;
        case SIDEBAR_ITEM_AGENT: icon_color = COLOR_GREEN;  break;
        case SIDEBAR_ITEM_SKILL: icon_color = COLOR_PURPLE; break;
        case SIDEBAR_ITEM_MEMORY:icon_color = COLOR_ACCENT; break;
        default:                 icon_color = COLOR_TEXT_DIM; break;
        }

        char icon_str[3];
        icon_str[0] = item->icon_char;
        icon_str[1] = ' ';
        icon_str[2] = '\0';

        if (item->is_section_header) {
            font_draw_string(fb, text_x, text_y, item->name,
                             text_color, 0, fnt);
        } else {
            font_draw_string(fb, text_x, text_y, icon_str,
                             icon_color, 0, fnt);
            font_draw_string(fb, text_x + fnt->width * 2, text_y,
                             item->name, text_color, 0, fnt);
        }

        y += item_h;
    }

    int total_h = sb->item_count * item_h + PADDING * 2;
    if (total_h > sb->bounds.h) {
        draw_scrollbar(fb,
                       sb->bounds.x + sb->bounds.w - 8,
                       sb->bounds.y,
                       sb->bounds.h,
                       sb->scroll_offset,
                       total_h,
                       sb->bounds.h);
    }
}

bool sidebar_handle_input(sidebar_t *sb, input_event_t *ev)
{
    if (!sb || !ev)
        return false;

    if (ev->type == 2) {
        if (!rect_contains(&sb->bounds, ev->mouse_x, ev->mouse_y))
            return false;

        const font_t *fnt = font_get_default();
        int item_h = fnt->height + MARGIN * 2;
        int rel_y = ev->mouse_y - sb->bounds.y - PADDING + sb->scroll_offset;
        int clicked = rel_y / item_h;

        if (clicked >= 0 && clicked < sb->item_count) {
            sb->selected_index = clicked;

            sidebar_item_t *item = &sb->items[clicked];
            if (item->is_section_header) {
                int section = -1;
                if (item->icon_char == 'F')
                    section = SIDEBAR_SECTION_FILES;
                else if (item->icon_char == 'A')
                    section = SIDEBAR_SECTION_AGENTS;
                else if (item->icon_char == 'S')
                    section = SIDEBAR_SECTION_SKILLS;
                else if (item->icon_char == 'M')
                    section = SIDEBAR_SECTION_MEMORY;

                if (section >= 0)
                    sb->sections_expanded[section] =
                        !sb->sections_expanded[section];
            }
            return true;
        }
        return false;
    }

    if (ev->type == 3) {
        const font_t *fnt = font_get_default();
        int item_h = fnt->height + MARGIN * 2;
        int total_h = sb->item_count * item_h + PADDING * 2;
        int max_scroll = total_h - sb->bounds.h;
        if (max_scroll < 0)
            max_scroll = 0;

        sb->scroll_offset -= ev->mouse_y * 2;
        if (sb->scroll_offset < 0)
            sb->scroll_offset = 0;
        if (sb->scroll_offset > max_scroll)
            sb->scroll_offset = max_scroll;
        return true;
    }

    if (ev->type != 1)
        return false;

    if (ev->keycode == KEY_UP) {
        if (sb->selected_index > 0)
            sb->selected_index--;
        return true;
    }
    if (ev->keycode == KEY_DOWN) {
        if (sb->selected_index < sb->item_count - 1)
            sb->selected_index++;
        return true;
    }
    if (ev->keycode == KEY_ENTER) {
        if (sb->selected_index >= 0 &&
            sb->selected_index < sb->item_count) {
            sidebar_item_t *item = &sb->items[sb->selected_index];
            if (item->is_section_header) {
                int section = -1;
                if (item->icon_char == 'F')
                    section = SIDEBAR_SECTION_FILES;
                else if (item->icon_char == 'A')
                    section = SIDEBAR_SECTION_AGENTS;
                else if (item->icon_char == 'S')
                    section = SIDEBAR_SECTION_SKILLS;
                else if (item->icon_char == 'M')
                    section = SIDEBAR_SECTION_MEMORY;

                if (section >= 0)
                    sb->sections_expanded[section] =
                        !sb->sections_expanded[section];
            } else if (item->type == SIDEBAR_ITEM_DIR) {
                item->is_expanded = !item->is_expanded;
            }
            return true;
        }
    }

    return false;
}
