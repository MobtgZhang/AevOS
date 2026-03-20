#pragma once

#include <aevos/types.h>

bool simd_detect(void);

void  simd_vec_add_f32(float *dst, const float *a, const float *b, size_t n);
void  simd_vec_mul_f32(float *dst, const float *a, const float *b, size_t n);
float simd_vec_dot_f32(const float *a, const float *b, size_t n);

void simd_mat_mul_f32(float *C, const float *A, const float *B,
                      size_t M, size_t N, size_t K);

void simd_softmax_f32(float *dst, const float *src, size_t n);
void simd_silu_f32(float *dst, const float *src, size_t n);
void simd_rmsnorm_f32(float *dst, const float *src, const float *weight, size_t n);

void simd_rope_f32(float *q, float *k, uint32_t n_embd, uint32_t n_head,
                   uint32_t pos, float theta);
