#include "shell.h"
#include "theme.h"
#include "font.h"
#include "widget.h"
#include "../kernel/arch/arch.h"
#include "../kernel/drivers/gpu_fb.h"
#include "../kernel/drivers/hid.h"
#include "../kernel/klog.h"
#include "../agent/agent_core.h"
#include "../lib/string.h"
#include <aevos/config.h>

static void shell_calculate_layout(ui_shell_t *shell)
{
    uint32_t screen_w = shell->fb->width;
    uint32_t screen_h = shell->fb->height;

    int sidebar_w = shell->show_sidebar ? SIDEBAR_WIDTH : 0;
    int content_x = sidebar_w;
    int content_w = (int)screen_w - sidebar_w;

    int terminal_h = shell->show_terminal ? (int)(screen_h * 30 / 100) : 0;
    int chat_h = (int)screen_h - TITLEBAR_HEIGHT - STATUSBAR_HEIGHT - terminal_h;
    int chat_y = TITLEBAR_HEIGHT;

    rect_t sidebar_bounds = {
        .x = 0,
        .y = TITLEBAR_HEIGHT,
        .w = sidebar_w,
        .h = (int)screen_h - TITLEBAR_HEIGHT - STATUSBAR_HEIGHT
    };

    rect_t chat_bounds = {
        .x = content_x,
        .y = chat_y,
        .w = content_w,
        .h = chat_h
    };

    rect_t term_bounds = {
        .x = content_x,
        .y = chat_y + chat_h,
        .w = content_w,
        .h = terminal_h
    };

    shell->sidebar.bounds  = sidebar_bounds;
    shell->chat.view_rect  = chat_bounds;
    shell->terminal.bounds = term_bounds;
}

void shell_init(ui_shell_t *shell, fb_ctx_t *fb, agent_t *agent)
{
    if (!shell || !fb)
        return;

    memset(shell, 0, sizeof(ui_shell_t));
    shell->fb            = fb;
    shell->agent         = agent;
    shell->active_panel  = PANEL_CHAT;
    shell->show_terminal = true;
    shell->show_sidebar  = true;
    shell->needs_redraw  = true;
    shell->uptime_ticks  = 0;
    shell->frame_count   = 0;
    shell->mouse_x       = (int32_t)(fb->width / 2);
    shell->mouse_y       = (int32_t)(fb->height / 2);
    shell->mouse_buttons = 0;
    shell->mouse_visible = true;
    shell->hover_panel   = PANEL_CHAT;

    font_init();
    shell_calculate_layout(shell);

    chat_view_init(&shell->chat, shell->chat.view_rect);
    sidebar_init(&shell->sidebar, shell->sidebar.bounds);
    terminal_init(&shell->terminal, shell->terminal.bounds);

    sidebar_populate(&shell->sidebar, agent);

    chat_view_add_message(&shell->chat, CHAT_ROLE_SYSTEM,
        "Welcome to AevOS v" AEVOS_VERSION_STRING
        " - Autonomous Evolving OS. "
        "I am your AI assistant. How can I help you today?");

    klog("[ui] layout ok (%ux%u)\n", fb->width, fb->height);
}

static void shell_render_titlebar(ui_shell_t *shell)
{
    fb_ctx_t *fb = shell->fb;
    const font_t *fnt = font_get_default();

    fb_draw_rect(0, 0, fb->width, TITLEBAR_HEIGHT, COLOR_SIDEBAR);
    fb_draw_rect(0, TITLEBAR_HEIGHT - 1, fb->width, 1, COLOR_DIVIDER);

    const char *title = "AevOS v" AEVOS_VERSION_STRING
                        " \xC4 Autonomous Evolving OS";
    int tw = font_measure_string(title, fnt);
    int tx = ((int)fb->width - tw) / 2;
    int ty = (TITLEBAR_HEIGHT - fnt->height) / 2;
    font_draw_string(fb, tx, ty, title, COLOR_TEXT, 0, fnt);

    const char *close_btn = "[X]";
    int cx = (int)fb->width - font_measure_string(close_btn, fnt) - PADDING;
    font_draw_string(fb, cx, ty, close_btn, COLOR_RED, 0, fnt);

    const char *min_btn = "[-]";
    int mx = cx - font_measure_string(min_btn, fnt) - PADDING;
    font_draw_string(fb, mx, ty, min_btn, COLOR_YELLOW, 0, fnt);
}

static void shell_render_statusbar(ui_shell_t *shell)
{
    fb_ctx_t *fb = shell->fb;
    const font_t *fnt = font_get_default();

    int sb_y = (int)fb->height - STATUSBAR_HEIGHT;
    fb_draw_rect(0, sb_y, fb->width, STATUSBAR_HEIGHT, COLOR_SIDEBAR);
    fb_draw_rect(0, sb_y, fb->width, 1, COLOR_DIVIDER);

    int text_y = sb_y + (STATUSBAR_HEIGHT - fnt->height) / 2;
    int x = PADDING;

    uint32_t uptime_s = (uint32_t)(shell->uptime_ticks / TIMER_FREQ_HZ);
    uint32_t hrs = uptime_s / 3600;
    uint32_t mins = (uptime_s % 3600) / 60;
    uint32_t secs = uptime_s % 60;

    char status[256];
    snprintf(status, sizeof(status),
             " Agent: %s | Skills: 3 | Uptime: %u:%02u:%02u | Mouse: %d,%d",
             (shell->agent && shell->agent->name[0]) ?
                 shell->agent->name : "default",
             hrs, mins, secs,
             shell->mouse_x, shell->mouse_y);
    font_draw_string(fb, x, text_y, status, COLOR_TEXT_DIM, 0, fnt);

    const char *mode;
    if (shell->active_panel == PANEL_CHAT)
        mode = "[CHAT]";
    else
        mode = "[TERM]";

    int mode_w = font_measure_string(mode, fnt);
    font_draw_string(fb, (int)fb->width - mode_w - PADDING,
                     text_y, mode, COLOR_ACCENT, 0, fnt);

    const char *keys = "F1:Sidebar F2:Terminal F3:Focus";
    int kw = font_measure_string(keys, fnt);
    font_draw_string(fb, (int)fb->width - mode_w - kw - PADDING * 3,
                     text_y, keys, COLOR_TEXT_DIM, 0, fnt);
}

static active_panel_t detect_hover_panel(ui_shell_t *shell)
{
    int mx = shell->mouse_x, my = shell->mouse_y;
    if (shell->show_terminal &&
        rect_contains(&shell->terminal.bounds, mx, my))
        return PANEL_TERMINAL;
    return PANEL_CHAT;
}

void shell_render(ui_shell_t *shell)
{
    if (!shell || !shell->fb)
        return;

    fb_clear(COLOR_BG);

    shell_render_titlebar(shell);

    if (shell->show_sidebar) {
        sidebar_render(&shell->sidebar, shell->fb);
    }

    chat_view_render(&shell->chat, shell->fb);

    if (shell->show_terminal) {
        terminal_render(&shell->terminal, shell->fb);
    }

    shell_render_statusbar(shell);

    if (shell->mouse_visible) {
        fb_draw_cursor(shell->mouse_x, shell->mouse_y, 0xFFFFFFFF);
    }

    fb_swap_buffers();
    shell->frame_count++;
    shell->needs_redraw = false;
}

void shell_handle_input(ui_shell_t *shell, input_event_t *ev)
{
    if (!shell || !ev)
        return;

    shell->needs_redraw = true;

    /* Track mouse position for all event types */
    if (ev->type == INPUT_MOUSE_MOVE || ev->type == INPUT_MOUSE_BUTTON) {
        shell->mouse_x = ev->mouse_x;
        shell->mouse_y = ev->mouse_y;
        shell->mouse_buttons = ev->mouse_buttons;
        shell->mouse_visible = true;

        if ((int32_t)shell->mouse_x < 0) shell->mouse_x = 0;
        if ((int32_t)shell->mouse_y < 0) shell->mouse_y = 0;
        if ((uint32_t)shell->mouse_x >= shell->fb->width)
            shell->mouse_x = (int32_t)(shell->fb->width - 1);
        if ((uint32_t)shell->mouse_y >= shell->fb->height)
            shell->mouse_y = (int32_t)(shell->fb->height - 1);

        shell->hover_panel = detect_hover_panel(shell);
    }

    /* Mouse click switches active panel */
    if (ev->type == INPUT_MOUSE_BUTTON && (ev->mouse_buttons & MOUSE_BTN_LEFT)) {
        shell->active_panel = shell->hover_panel;

        if (shell->show_sidebar &&
            rect_contains(&shell->sidebar.bounds,
                          ev->mouse_x, ev->mouse_y)) {
            sidebar_handle_input(&shell->sidebar, ev);
            sidebar_populate(&shell->sidebar, shell->agent);
            return;
        }
    }

    /* Keyboard events */
    if (ev->type == INPUT_KEY_RELEASE) {
        if (ev->keycode == KEY_F1) {
            shell->show_sidebar = !shell->show_sidebar;
            shell_calculate_layout(shell);
            sidebar_populate(&shell->sidebar, shell->agent);
            return;
        }

        if (ev->keycode == KEY_F2) {
            shell->show_terminal = !shell->show_terminal;
            shell_calculate_layout(shell);
            return;
        }

        if (ev->keycode == KEY_F3) {
            if (shell->active_panel == PANEL_CHAT)
                shell->active_panel = PANEL_TERMINAL;
            else
                shell->active_panel = PANEL_CHAT;
            return;
        }

        if (ev->modifiers & MOD_CTRL) {
            if (ev->keycode == KEY_N) {
                terminal_print(&shell->terminal, "Creating new agent...");
                return;
            }
            if (ev->keycode == KEY_E) {
                terminal_print(&shell->terminal,
                               "Skill evolution dialog (not yet implemented)");
                return;
            }
        }
    }

    /* Forward mouse scroll to sidebar */
    if (shell->show_sidebar &&
        (ev->type == INPUT_MOUSE_MOVE || ev->type == INPUT_MOUSE_BUTTON)) {
        if (rect_contains(&shell->sidebar.bounds,
                          ev->mouse_x, ev->mouse_y)) {
            sidebar_handle_input(&shell->sidebar, ev);
            return;
        }
    }

    /* Forward to active panel */
    if (shell->active_panel == PANEL_CHAT) {
        if (chat_view_handle_input(&shell->chat, ev)) {
            if (ev->keycode == KEY_ENTER && ev->type == INPUT_KEY_RELEASE) {
                int count = shell->chat.message_count;
                if (count > 0 &&
                    shell->chat.messages[count - 1].role == CHAT_ROLE_USER) {
                    shell_on_user_message(shell,
                        shell->chat.messages[count - 1].text);
                }
            }
            return;
        }
    } else if (shell->active_panel == PANEL_TERMINAL) {
        if (terminal_handle_input(&shell->terminal, ev))
            return;
    }
}

void shell_on_user_message(ui_shell_t *shell, const char *text)
{
    if (!shell || !text)
        return;

    klog("User: %s\n", text);

    shell->chat.is_ai_typing = true;
    shell->needs_redraw = true;

    if (shell->agent) {
        agent_process_input(shell->agent, text);
    }

    chat_view_add_message(&shell->chat, CHAT_ROLE_ASSISTANT,
        "I received your message. The LLM runtime will process it "
        "and provide a response. (LLM integration pending)");
    shell->chat.is_ai_typing = false;
    shell->needs_redraw = true;
}

void shell_main_loop(ui_shell_t *shell)
{
    if (!shell)
        return;

    klog("Starting UI main loop\n");

    shell->needs_redraw = true;

    for (;;) {
        input_event_t ev;
        while (input_poll(&ev)) {
            shell_handle_input(shell, &ev);
        }

        shell->uptime_ticks++;

        if (shell->uptime_ticks % (TIMER_FREQ_HZ * 5) == 0) {
            if (shell->agent)
                agent_tick(shell->agent);
        }

        if (shell->uptime_ticks % (TIMER_FREQ_HZ / 30) == 0) {
            shell->terminal.blink_counter++;
            shell->needs_redraw = true;
        }

        if (shell->needs_redraw) {
            shell_render(shell);
        }

        arch_idle();
    }
}
