#pragma once

#include <aevos/types.h>

/* Returns 0 if virtio-net attached to the IP stack, -1 if none. */
int virtio_net_init(void);

/* Drain RX virtqueue and feed frames into net_process_frame (call from UI loop). */
void virtio_net_poll(void);

bool virtio_net_available(void);
