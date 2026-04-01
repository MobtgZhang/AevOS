/*
 * 标准 Wayland wire 兼容占位：未来在此接入套接字监听、wl_display 握手与 SHM buffer。
 * 当前内核合成器使用 aevos_wl_* 语义层（wl_compositor.c + wl_compositor_service.c）。
 */

#include <aevos/types.h>

typedef struct {
    uint32_t wl_display_serial;
    uint16_t stub_object_id;
} aevos_wl_wire_future_placeholder_t;

void aevos_wl_wire_future_init_placeholder(aevos_wl_wire_future_placeholder_t *p)
{
    if (!p)
        return;
    p->wl_display_serial = 1;
    p->stub_object_id    = 1;
}
