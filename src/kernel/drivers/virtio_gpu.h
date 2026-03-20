#pragma once

#include <aevos/types.h>

/*
 * Minimal virtio-GPU 2D driver.
 * Provides display initialization and framebuffer flush for platforms
 * that use virtio-gpu-pci (e.g., LoongArch QEMU virt).
 */

int  virtio_gpu_init(uint64_t fb_phys, uint32_t width, uint32_t height);
void virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
bool virtio_gpu_available(void);
