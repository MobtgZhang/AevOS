#pragma once

#include <aevos/types.h>

#define QK4_0 32
#define QK8_0 32
#define QK_K  256

typedef uint16_t float16_t;

typedef struct PACKED {
    float16_t delta;
    uint8_t   nibbles[QK4_0 / 2];
} q4_0_block_t;

typedef struct PACKED {
    float16_t delta;
    int8_t    quants[QK8_0];
} q8_0_block_t;

typedef struct PACKED {
    float16_t d;
    float16_t dmin;
    uint8_t   scales[12];
    uint8_t   nibbles[QK_K / 2];
} q4_k_block_t;

float     f16_to_f32(float16_t h);
float16_t f32_to_f16(float f);

void quantize_f32_to_q4_0(const float *src, q4_0_block_t *dst, size_t n);
void dequantize_q4_0_to_f32(const q4_0_block_t *src, float *dst, size_t n);

void quantize_f32_to_q8_0(const float *src, q8_0_block_t *dst, size_t n);
void dequantize_q8_0_to_f32(const q8_0_block_t *src, float *dst, size_t n);

float vec_dot_q4_0_q8_0(const q4_0_block_t *a, const q8_0_block_t *b, size_t n);
float vec_dot_q8_0_q8_0(const q8_0_block_t *a, const q8_0_block_t *b, size_t n);
float vec_dot_q4_k_q8_0(const q4_k_block_t *a, const q8_0_block_t *b, size_t n);
