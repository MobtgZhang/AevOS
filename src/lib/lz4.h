#pragma once

#include <aevos/types.h>

/*
 * Simplified LZ4 block compression / decompression.
 * Returns compressed/decompressed size on success, -1 on error.
 */
ssize_t lz4_compress(const void *src, size_t src_size,
                     void *dst, size_t dst_capacity);

ssize_t lz4_decompress(const void *src, size_t src_size,
                       void *dst, size_t dst_capacity);
