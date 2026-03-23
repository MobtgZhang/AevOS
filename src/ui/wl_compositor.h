#pragma once

/*
 * Wayland 风格的内核内合成器（非 wire 协议）
 *
 * 标准 Wayland 依赖 Unix 域套接字、共享内存、DRM/KMS 以及用户态客户端，
 * 无法在 AevOS 裸机环境中直接运行。本模块用与 Wayland 相近的概念
 *（wl_output / wl_surface / compositor 提交顺序）组织帧绘制，
 * 便于将来若引入用户态或主机端客户端时替换后端。
 */

#include <aevos/types.h>
#include "aevos_wl_protocol.h"

struct ui_shell;
typedef struct ui_shell ui_shell_t;

struct aevos_wl_compositor;
typedef struct aevos_wl_compositor aevos_wl_compositor_t;

#define AEVOS_WL_MAX_SURFACES 16

typedef void (*aevos_wl_surface_paint_fn)(aevos_wl_compositor_t *comp);

typedef struct aevos_wl_surface {
    const char               *role;
    int32_t                   z_order;
    bool                      attached;
    aevos_wl_surface_paint_fn paint;
} aevos_wl_surface_t;

typedef struct aevos_wl_output {
    uint32_t width_px;
    uint32_t height_px;
    uint32_t refresh_mHz;
} aevos_wl_output_t;

struct aevos_wl_compositor {
    ui_shell_t          *shell;
    aevos_wl_output_t    output;
    aevos_wl_surface_t   surfaces[AEVOS_WL_MAX_SURFACES];
    size_t               surface_count;
};

void aevos_wl_compositor_init(aevos_wl_compositor_t *comp, ui_shell_t *shell);
void aevos_wl_compositor_refresh_output(aevos_wl_compositor_t *comp);
void aevos_wl_compositor_damage_full(aevos_wl_compositor_t *comp);
void aevos_wl_compositor_present(aevos_wl_compositor_t *comp);
