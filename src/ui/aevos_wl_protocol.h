#pragma once

/*
 * AevOS Wayland 风格展示协议（消息层规范）
 *
 * 与内核内 wl_compositor（帧合成）的关系：
 *   - wl_compositor.c：对象树 + z-order + paint()，负责把各 surface 画到帧缓冲。
 *   - 本头文件：将来用于「客户端 ⇄ 合成服务」的 NDJSON/共享内存 等传输时的
 *     逻辑对象 ID 与事件编号，命名对齐 Wayland 习惯（wl_display / wl_surface /
 *     xdg_toplevel 等），便于在主机侧用 TypeScript 实现同款 UI 并与裸机内核对接。
 *
 * 线格式（建议）：每行一条 UTF-8 JSON，字段 type + payload。
 * 主机参考实现：devtools/cursor-workbench/
 *
 * 与标准 Wayland wire 的差距（路线图，见 wl_wire_future_stub.c）：
 *   - 尚无 Unix 套接字对象传输、wl_display 扫描器生成协议；
 *   - 计划子集：wl_compositor / wl_surface / wl_shm / xdg_toplevel 的 fd+opcode 映射。
 */

#include <aevos/types.h>

/* 逻辑对象：客户端申请 ID，服务端在 registry 中回显 global 事件 */
#define AEVOS_WL_DISPLAY_ID      1u
#define AEVOS_WL_REGISTRY_ID     2u
#define AEVOS_WL_COMPOSITOR_ID   3u
#define AEVOS_WL_OUTPUT_ID       4u
#define AEVOS_WL_SEAT_ID         5u
#define AEVOS_WL_SHM_ID          6u
#define AEVOS_XDG_WM_BASE_ID     7u

typedef enum {
    AEVOS_WL_EVENT_REGISTRY_GLOBAL = 1,
    AEVOS_WL_EVENT_OUTPUT_GEOMETRY,
    AEVOS_WL_EVENT_POINTER_MOTION,
    AEVOS_WL_EVENT_POINTER_BUTTON,
    AEVOS_WL_EVENT_KEYBOARD_KEY,
    AEVOS_WL_EVENT_CONFIGURE,
    AEVOS_WL_EVENT_FRAME_DONE,
    AEVOS_WL_EVENT_SURFACE_DAMAGE,
    AEVOS_WL_EVENT_OUTPUT_MODE,
} aevos_wl_wire_event_t;

typedef enum {
    AEVOS_WL_REQ_REGISTRY_BIND = 1,
    AEVOS_WL_REQ_SURFACE_ATTACH,
    AEVOS_WL_REQ_SURFACE_COMMIT,
    AEVOS_WL_REQ_XDG_TOPLEVEL_SET_TITLE,
    AEVOS_WL_REQ_SUBSURFACE_PLACE_ABOVE,
    AEVOS_WL_REQ_COMPOSITOR_PING,
    AEVOS_WL_REQ_OUTPUT_SYNC,
} aevos_wl_wire_request_t;

/*
 * 服务端 → 客户端：帧提交后附带 damage 矩形（全屏时可单矩形覆盖 output）
 */
typedef struct {
    int32_t  x;
    int32_t  y;
    uint32_t width;
    uint32_t height;
} aevos_wl_damage_rect_t;

/*
 * Shell 集成：与侧边栏 / 聊天 / 伪终端共享同一事件泵时可携带 panel id。
 */
typedef enum {
    AEVOS_SHELL_PANEL_SIDEBAR = 0,
    AEVOS_SHELL_PANEL_CHAT    = 1,
    AEVOS_SHELL_PANEL_TERM    = 2,
    AEVOS_SHELL_PANEL_SANDBOX = 3,
} aevos_shell_panel_id_t;
