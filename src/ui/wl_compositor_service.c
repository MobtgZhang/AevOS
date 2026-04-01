#include "wl_compositor_service.h"
#include "lib/string.h"

void aevos_wl_service_init(aevos_wl_service_state_t *st)
{
    if (!st)
        return;
    memset(st, 0, sizeof(*st));
}

void aevos_wl_service_on_present(aevos_wl_service_state_t *st,
                                 aevos_wl_compositor_t *comp)
{
    if (!st || !comp)
        return;
    st->frame_seq++;
    st->last_damage_count = 1;
    st->last_damage.x      = 0;
    st->last_damage.y      = 0;
    st->last_damage.width  = comp->output.width_px;
    st->last_damage.height = comp->output.height_px;
    (void)AEVOS_WL_EVENT_FRAME_DONE;
}

bool aevos_wl_service_apply_wire_request(aevos_wl_service_state_t *st,
                                         aevos_wl_wire_request_t req,
                                         uint32_t arg)
{
    (void)arg;
    if (!st)
        return false;
    switch (req) {
    case AEVOS_WL_REQ_COMPOSITOR_PING:
    case AEVOS_WL_REQ_OUTPUT_SYNC:
        return true;
    default:
        return false;
    }
}
