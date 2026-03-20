#include "quantize.h"
#include "lib/string.h"

/* ── float16 ⇔ float32 conversion ──────────────────────────── */

float f16_to_f32(float16_t h) {
    uint32_t sign = ((uint32_t)h & 0x8000u) << 16;
    uint32_t expo = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x03FF;
    uint32_t f;

    if (expo == 0) {
        if (mant == 0) {
            f = sign;
        } else {
            expo = 1;
            while (!(mant & 0x0400)) { mant <<= 1; expo--; }
            mant &= 0x03FF;
            f = sign | ((expo + 112) << 23) | (mant << 13);
        }
    } else if (expo == 31) {
        f = sign | 0x7F800000u | (mant << 13);
    } else {
        f = sign | ((expo + 112) << 23) | (mant << 13);
    }

    float result;
    memcpy(&result, &f, sizeof(float));
    return result;
}

float16_t f32_to_f16(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(float));

    uint32_t sign = (bits >> 16) & 0x8000;
    int32_t  expo = ((bits >> 23) & 0xFF) - 127;
    uint32_t mant = bits & 0x007FFFFF;

    if (expo > 15) {
        return (float16_t)(sign | 0x7C00);
    }
    if (expo < -14) {
        if (expo < -24) return (float16_t)sign;
        mant |= 0x00800000;
        uint32_t shift = (uint32_t)(-expo - 1);
        mant >>= (shift + 13);
        return (float16_t)(sign | (mant & 0x03FF));
    }

    return (float16_t)(sign | ((expo + 15) << 10) | (mant >> 13));
}

/* ── helpers ───────────────────────────────────────────────── */

static float absf(float x) { return x >= 0.0f ? x : -x; }

static float maxf(float a, float b) { return a > b ? a : b; }

/* ── Quantize F32 → Q4_0 ──────────────────────────────────── */

void quantize_f32_to_q4_0(const float *src, q4_0_block_t *dst, size_t n) {
    size_t nblocks = n / QK4_0;

    for (size_t b = 0; b < nblocks; b++) {
        const float *block = src + b * QK4_0;

        float amax = 0.0f;
        for (int i = 0; i < QK4_0; i++)
            amax = maxf(amax, absf(block[i]));

        float d = amax / 7.0f;
        float id = (d != 0.0f) ? 1.0f / d : 0.0f;

        dst[b].delta = f32_to_f16(d);

        for (int i = 0; i < QK4_0 / 2; i++) {
            float x0 = block[2 * i]     * id;
            float x1 = block[2 * i + 1] * id;
            int v0 = (int)(x0 + 8.5f);
            int v1 = (int)(x1 + 8.5f);
            if (v0 < 0) v0 = 0;
            if (v0 > 15) v0 = 15;
            if (v1 < 0) v1 = 0;
            if (v1 > 15) v1 = 15;
            dst[b].nibbles[i] = (uint8_t)((v0 & 0xF) | ((v1 & 0xF) << 4));
        }
    }
}

/* ── Dequantize Q4_0 → F32 ────────────────────────────────── */

void dequantize_q4_0_to_f32(const q4_0_block_t *src, float *dst, size_t n) {
    size_t nblocks = n / QK4_0;

    for (size_t b = 0; b < nblocks; b++) {
        float d = f16_to_f32(src[b].delta);
        float *out = dst + b * QK4_0;

        for (int i = 0; i < QK4_0 / 2; i++) {
            uint8_t packed = src[b].nibbles[i];
            int v0 = (packed & 0xF) - 8;
            int v1 = ((packed >> 4) & 0xF) - 8;
            out[2 * i]     = (float)v0 * d;
            out[2 * i + 1] = (float)v1 * d;
        }
    }
}

/* ── Quantize F32 → Q8_0 ──────────────────────────────────── */

void quantize_f32_to_q8_0(const float *src, q8_0_block_t *dst, size_t n) {
    size_t nblocks = n / QK8_0;

    for (size_t b = 0; b < nblocks; b++) {
        const float *block = src + b * QK8_0;

        float amax = 0.0f;
        for (int i = 0; i < QK8_0; i++)
            amax = maxf(amax, absf(block[i]));

        float d = amax / 127.0f;
        float id = (d != 0.0f) ? 1.0f / d : 0.0f;

        dst[b].delta = f32_to_f16(d);

        for (int i = 0; i < QK8_0; i++) {
            float v = block[i] * id;
            int q = (int)(v + (v >= 0 ? 0.5f : -0.5f));
            if (q < -128) q = -128;
            if (q >  127) q =  127;
            dst[b].quants[i] = (int8_t)q;
        }
    }
}

/* ── Dequantize Q8_0 → F32 ────────────────────────────────── */

void dequantize_q8_0_to_f32(const q8_0_block_t *src, float *dst, size_t n) {
    size_t nblocks = n / QK8_0;

    for (size_t b = 0; b < nblocks; b++) {
        float d = f16_to_f32(src[b].delta);
        float *out = dst + b * QK8_0;

        for (int i = 0; i < QK8_0; i++)
            out[i] = (float)src[b].quants[i] * d;
    }
}

/* ── Quantized dot product: Q4_0 · Q8_0 ──────────────────── */

float vec_dot_q4_0_q8_0(const q4_0_block_t *a, const q8_0_block_t *b, size_t n) {
    size_t nblocks = n / QK4_0;
    float sum = 0.0f;

    for (size_t bl = 0; bl < nblocks; bl++) {
        float da = f16_to_f32(a[bl].delta);
        float db = f16_to_f32(b[bl].delta);
        int32_t isum = 0;

        for (int i = 0; i < QK4_0 / 2; i++) {
            uint8_t packed = a[bl].nibbles[i];
            int v0 = (packed & 0xF) - 8;
            int v1 = ((packed >> 4) & 0xF) - 8;
            isum += v0 * (int)b[bl].quants[2 * i];
            isum += v1 * (int)b[bl].quants[2 * i + 1];
        }
        sum += da * db * (float)isum;
    }
    return sum;
}

/* ── Quantized dot product: Q4_K · Q8_0 ──────────────────── */

float vec_dot_q4_k_q8_0(const q4_k_block_t *a, const q8_0_block_t *b, size_t n) {
    size_t n_super = n / QK_K;
    size_t q8_per_super = QK_K / QK8_0;
    float total = 0.0f;

    for (size_t s = 0; s < n_super; s++) {
        float d    = f16_to_f32(a[s].d);
        float dmin = f16_to_f32(a[s].dmin);

        for (size_t sub = 0; sub < 8; sub++) {
            uint8_t sc_byte = a[s].scales[sub < 4 ? sub : sub + 2];
            float sub_d   = d    * (float)(sc_byte & 0x3F);
            float sub_min = dmin * (float)((sc_byte >> 4) & 0x3);

            size_t sub_off = sub * (QK_K / 8);
            size_t q8_base = s * q8_per_super + sub * (QK_K / 8 / QK8_0);

            for (size_t i = 0; i < QK_K / 8; i += 2) {
                uint8_t packed = a[s].nibbles[(sub_off + i) / 2];
                int v0 = (packed & 0xF);
                int v1 = ((packed >> 4) & 0xF);

                size_t q8_block = q8_base + (i / QK8_0);
                size_t q8_idx   = i % QK8_0;

                if (q8_block < n_super * q8_per_super) {
                    float db = f16_to_f32(b[q8_block].delta);
                    int b0 = b[q8_block].quants[q8_idx];
                    int b1 = b[q8_block].quants[q8_idx + 1];

                    total += sub_d * db * (float)(v0 * b0 + v1 * b1);
                    total -= sub_min * db * (float)(b0 + b1);
                }
            }
        }
    }
    return total;
}
