#pragma once

#include <aevos/types.h>
#include "aevos_wl_protocol.h"
#include "wl_compositor.h"

/*
 * Wayland 语义合成服务：与标准 wl_display wire 分离，负责帧序与 damage 记账，
 * 供 shell / 未来 NDJSON 桥使用。
 */

typedef struct {
    uint64_t frame_seq;
    uint32_t last_damage_count;
    aevos_wl_damage_rect_t last_damage;
} aevos_wl_service_state_t;

void aevos_wl_service_init(aevos_wl_service_state_t *st);

void aevos_wl_service_on_present(aevos_wl_service_state_t *st,
                                 aevos_wl_compositor_t *comp);

bool aevos_wl_service_apply_wire_request(aevos_wl_service_state_t *st,
                                         aevos_wl_wire_request_t req,
                                         uint32_t arg);
