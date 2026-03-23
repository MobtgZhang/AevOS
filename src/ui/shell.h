#pragma once

#include <aevos/types.h>
#include "chat_view.h"
#include "sidebar.h"
#include "terminal.h"
#include "wl_compositor.h"

typedef struct fb_ctx fb_ctx_t;
typedef struct input_event input_event_t;
typedef struct agent agent_t;

typedef enum {
    PANEL_CHAT,
    PANEL_TERMINAL
} active_panel_t;

typedef struct ui_shell {
    fb_ctx_t               *fb;
    agent_t                *agent;
    chat_view_t             chat;
    sidebar_t               sidebar;
    terminal_t              terminal;
    active_panel_t          active_panel;
    bool                    show_terminal;
    bool                    show_sidebar;
    bool                    needs_redraw;
    uint64_t                uptime_ticks;
    uint32_t                frame_count;

    int32_t                 mouse_x;
    int32_t                 mouse_y;
    uint8_t                 mouse_buttons;
    bool                    mouse_visible;
    active_panel_t          hover_panel;
    aevos_wl_compositor_t   compositor;
} ui_shell_t;

void shell_layer_titlebar(ui_shell_t *shell);
void shell_layer_statusbar(ui_shell_t *shell);

void shell_init(ui_shell_t *shell, fb_ctx_t *fb, agent_t *agent);
void shell_render(ui_shell_t *shell);
void shell_handle_input(ui_shell_t *shell, input_event_t *ev);
void shell_main_loop(ui_shell_t *shell);
void shell_on_user_message(ui_shell_t *shell, const char *text);
