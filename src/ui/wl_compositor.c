#include "wl_compositor.h"
#include "shell.h"
#include "theme.h"
#include "../kernel/drivers/gpu_fb.h"
#include "../lib/string.h"

static void wl_paint_background(aevos_wl_compositor_t *comp)
{
    (void)comp;
    fb_clear(COLOR_BG);
}

static void wl_paint_titlebar(aevos_wl_compositor_t *comp)
{
    shell_layer_titlebar(comp->shell);
}

static void wl_paint_sidebar(aevos_wl_compositor_t *comp)
{
    ui_shell_t *sh = comp->shell;
    if (sh->show_sidebar)
        sidebar_render(&sh->sidebar, sh->fb);
}

static void wl_paint_chat(aevos_wl_compositor_t *comp)
{
    ui_shell_t *sh = comp->shell;
    chat_view_render(&sh->chat, sh->fb);
}

static void wl_paint_terminal(aevos_wl_compositor_t *comp)
{
    ui_shell_t *sh = comp->shell;
    if (sh->show_terminal)
        terminal_render(&sh->terminal, sh->fb);
}

static void wl_paint_statusbar(aevos_wl_compositor_t *comp)
{
    shell_layer_statusbar(comp->shell);
}

static void wl_paint_cursor(aevos_wl_compositor_t *comp)
{
    ui_shell_t *sh = comp->shell;
    if (sh->mouse_visible)
        fb_draw_cursor(sh->mouse_x, sh->mouse_y, 0xFFFFFFFF);
}

static void wl_register_surface(aevos_wl_compositor_t *comp,
                                const char *role,
                                int32_t z,
                                aevos_wl_surface_paint_fn paint)
{
    if (comp->surface_count >= AEVOS_WL_MAX_SURFACES)
        return;

    aevos_wl_surface_t *s = &comp->surfaces[comp->surface_count++];
    s->role      = role;
    s->z_order   = z;
    s->attached  = true;
    s->paint     = paint;
}

static void wl_sort_surfaces_by_z(aevos_wl_compositor_t *comp)
{
    size_t n = comp->surface_count;
    for (size_t i = 0; i + 1 < n; i++) {
        for (size_t j = 0; j + 1 < n - i; j++) {
            if (comp->surfaces[j].z_order > comp->surfaces[j + 1].z_order) {
                aevos_wl_surface_t tmp = comp->surfaces[j];
                comp->surfaces[j]     = comp->surfaces[j + 1];
                comp->surfaces[j + 1] = tmp;
            }
        }
    }
}

void aevos_wl_compositor_init(aevos_wl_compositor_t *comp, ui_shell_t *shell)
{
    if (!comp || !shell)
        return;

    memset(comp, 0, sizeof(*comp));
    comp->shell = shell;

    if (shell->fb) {
        comp->output.width_px   = shell->fb->width;
        comp->output.height_px  = shell->fb->height;
        /* wl_output.refresh：常见桌面为 60Hz → 60000 mHz（与内核 tick 频率无关） */
        comp->output.refresh_mHz = 60u * 1000u;
    }

    wl_register_surface(comp, "wl_background", 0, wl_paint_background);
    wl_register_surface(comp, "xdg_toplevel_chrome", 10, wl_paint_titlebar);
    wl_register_surface(comp, "wl_subsurface_sidebar", 20, wl_paint_sidebar);
    wl_register_surface(comp, "xdg_toplevel_chat", 30, wl_paint_chat);
    wl_register_surface(comp, "xdg_toplevel_terminal", 40, wl_paint_terminal);
    wl_register_surface(comp, "wl_subsurface_status", 50, wl_paint_statusbar);
    wl_register_surface(comp, "wl_cursor", 60, wl_paint_cursor);

    wl_sort_surfaces_by_z(comp);
}

void aevos_wl_compositor_refresh_output(aevos_wl_compositor_t *comp)
{
    if (!comp || !comp->shell || !comp->shell->fb)
        return;

    comp->output.width_px    = comp->shell->fb->width;
    comp->output.height_px   = comp->shell->fb->height;
    comp->output.refresh_mHz = 60u * 1000u;
}

void aevos_wl_compositor_damage_full(aevos_wl_compositor_t *comp)
{
    (void)comp;
    /* 帧缓冲全量合成；细粒度 damage 可在后续按矩形裁剪扩展 */
}

void aevos_wl_compositor_present(aevos_wl_compositor_t *comp)
{
    if (!comp || !comp->shell || !comp->shell->fb)
        return;

    aevos_wl_compositor_refresh_output(comp);
    aevos_wl_compositor_damage_full(comp);

    for (size_t i = 0; i < comp->surface_count; i++) {
        aevos_wl_surface_t *s = &comp->surfaces[i];
        if (s->attached && s->paint)
            s->paint(comp);
    }

    fb_swap_buffers();
    comp->shell->frame_count++;
    comp->shell->needs_redraw = false;
}
