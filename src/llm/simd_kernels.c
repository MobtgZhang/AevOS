#include "simd_kernels.h"
#include "../lib/string.h"

#if defined(__x86_64__) && defined(__AVX2__)
#include <immintrin.h>
#define SIMD_AVX2 1
#else
#define SIMD_AVX2 0
#endif

#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(AEVOS_KERNEL)
#include <arm_neon.h>
#define SIMD_NEON 1
#else
#define SIMD_NEON 0
#endif

static bool avx2_available = false;
static bool avx2_checked   = false;

/* ── Fast math approximations (bare-metal, no libm) ─────────────── */

static float fast_expf(float x) {
    if (x > 88.72f)  return 3.4028235e+38f;
    if (x < -87.33f) return 0.0f;
    float kf = x * 1.4426950408889634f;
    int32_t k = (int32_t)(kf + (kf >= 0 ? 0.5f : -0.5f));
    float r  = x - (float)k * 0.6931471805599453f;
    float r2 = r * r;
    float p  = 1.0f + r + r2 * 0.5f + r2 * r * (1.0f / 6.0f)
             + r2 * r2 * (1.0f / 24.0f) + r2 * r2 * r * (1.0f / 120.0f);
    union { float f; int32_t i; } u = { p };
    u.i += k << 23;
    return u.f;
}

static float fast_sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    union { float f; uint32_t i; } u = { x };
    u.i = (u.i >> 1) + 0x1FC00000u;
    u.f = 0.5f * (u.f + x / u.f);
    u.f = 0.5f * (u.f + x / u.f);
    return u.f;
}

static float fast_sinf(float x) {
    const float PI  = 3.14159265358979f;
    const float PI2 = 6.28318530717959f;
    x = x - (int)(x / PI2) * PI2;
    if (x > PI)  x -= PI2;
    if (x < -PI) x += PI2;
    float x2 = x * x;
    return x * (1.0f - x2 * (1.0f / 6.0f)
        * (1.0f - x2 * (1.0f / 20.0f)
        * (1.0f - x2 * (1.0f / 42.0f)
        * (1.0f - x2 * (1.0f / 72.0f)))));
}

static float fast_cosf(float x) {
    return fast_sinf(x + 1.5707963267948966f);
}

static float fast_logf(float x) {
    if (x <= 0.0f) return -88.0f;
    union { float f; uint32_t i; } u = { x };
    float log2 = (float)((int32_t)u.i - (int32_t)0x3F800000) / (float)(1 << 23);
    return log2 * 0.6931471805599453f;
}

static float fast_powf(float base, float exp) {
    return fast_expf(exp * fast_logf(base));
}

/* ── Runtime CPUID detection ──────────────────────────────────── */

bool simd_detect(void) {
    if (avx2_checked) return avx2_available;
    avx2_checked = true;
#if SIMD_AVX2 && defined(__x86_64__)
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(7), "c"(0)
    );
    avx2_available = (ebx & (1u << 5)) != 0;
#elif SIMD_NEON
    avx2_available = false;
#else
    avx2_available = false;
#endif
    return avx2_available;
}

/* ── Horizontal sum helpers (AVX2) ───────────────────────────── */

#if SIMD_AVX2
static inline float hsum256(__m256 v) {
    __m128 lo  = _mm256_castps256_ps128(v);
    __m128 hi  = _mm256_extractf128_ps(v, 1);
    lo = _mm_add_ps(lo, hi);
    __m128 shuf = _mm_movehdup_ps(lo);
    __m128 sums = _mm_add_ps(lo, shuf);
    shuf = _mm_movehl_ps(shuf, sums);
    sums = _mm_add_ss(sums, shuf);
    return _mm_cvtss_f32(sums);
}
#endif

/* ── Vector add ─────────────────────────────────────────────── */

void simd_vec_add_f32(float *dst, const float *a, const float *b, size_t n) {
#if SIMD_AVX2
    if (avx2_available || simd_detect()) {
        size_t i = 0;
        for (; i + 7 < n; i += 8) {
            __m256 va = _mm256_loadu_ps(a + i);
            __m256 vb = _mm256_loadu_ps(b + i);
            _mm256_storeu_ps(dst + i, _mm256_add_ps(va, vb));
        }
        for (; i < n; i++) dst[i] = a[i] + b[i];
        return;
    }
#endif
    for (size_t i = 0; i < n; i++) dst[i] = a[i] + b[i];
}

/* ── Vector element-wise multiply ──────────────────────────── */

void simd_vec_mul_f32(float *dst, const float *a, const float *b, size_t n) {
#if SIMD_AVX2
    if (avx2_available || simd_detect()) {
        size_t i = 0;
        for (; i + 7 < n; i += 8) {
            __m256 va = _mm256_loadu_ps(a + i);
            __m256 vb = _mm256_loadu_ps(b + i);
            _mm256_storeu_ps(dst + i, _mm256_mul_ps(va, vb));
        }
        for (; i < n; i++) dst[i] = a[i] * b[i];
        return;
    }
#endif
    for (size_t i = 0; i < n; i++) dst[i] = a[i] * b[i];
}

/* ── Dot product ───────────────────────────────────────────── */

float simd_vec_dot_f32(const float *a, const float *b, size_t n) {
#if SIMD_AVX2
    if (avx2_available || simd_detect()) {
        __m256 acc = _mm256_setzero_ps();
        size_t i = 0;
        for (; i + 7 < n; i += 8) {
            __m256 va = _mm256_loadu_ps(a + i);
            __m256 vb = _mm256_loadu_ps(b + i);
            acc = _mm256_fmadd_ps(va, vb, acc);
        }
        float sum = hsum256(acc);
        for (; i < n; i++) sum += a[i] * b[i];
        return sum;
    }
#endif
    float sum = 0.0f;
    for (size_t i = 0; i < n; i++) sum += a[i] * b[i];
    return sum;
}

/* ── Matrix multiply: C[M,N] = A[M,K] * B[K,N] ──────────── */

void simd_mat_mul_f32(float *C, const float *A, const float *B,
                      size_t M, size_t N, size_t K) {
#if SIMD_AVX2
    if (avx2_available || simd_detect()) {
        for (size_t i = 0; i < M; i++) {
            size_t j = 0;
            for (; j + 7 < N; j += 8) {
                __m256 acc = _mm256_setzero_ps();
                for (size_t k = 0; k < K; k++) {
                    __m256 a_val = _mm256_set1_ps(A[i * K + k]);
                    __m256 b_val = _mm256_loadu_ps(&B[k * N + j]);
                    acc = _mm256_fmadd_ps(a_val, b_val, acc);
                }
                _mm256_storeu_ps(&C[i * N + j], acc);
            }
            for (; j < N; j++) {
                float s = 0.0f;
                for (size_t k = 0; k < K; k++)
                    s += A[i * K + k] * B[k * N + j];
                C[i * N + j] = s;
            }
        }
        return;
    }
#endif
    for (size_t i = 0; i < M; i++) {
        for (size_t j = 0; j < N; j++) {
            float s = 0.0f;
            for (size_t k = 0; k < K; k++)
                s += A[i * K + k] * B[k * N + j];
            C[i * N + j] = s;
        }
    }
}

/* ── Softmax ─────────────────────────────────────────────── */

void simd_softmax_f32(float *dst, const float *src, size_t n) {
    if (n == 0) return;

    float max_val = src[0];
#if SIMD_AVX2
    if (avx2_available || simd_detect()) {
        __m256 vmax = _mm256_set1_ps(src[0]);
        size_t i = 0;
        for (; i + 7 < n; i += 8) {
            __m256 v = _mm256_loadu_ps(src + i);
            vmax = _mm256_max_ps(vmax, v);
        }
        max_val = hsum256(_mm256_max_ps(vmax, _mm256_permute2f128_ps(vmax, vmax, 1)));
        /* hsum256 gives sum; we need max across lanes instead */
        float tmp[8];
        _mm256_storeu_ps(tmp, vmax);
        max_val = tmp[0];
        for (int j = 1; j < 8; j++)
            if (tmp[j] > max_val) max_val = tmp[j];
        for (; i < n; i++)
            if (src[i] > max_val) max_val = src[i];

        __m256 vm = _mm256_set1_ps(max_val);
        __m256 vsum = _mm256_setzero_ps();
        i = 0;
        for (; i + 7 < n; i += 8) {
            __m256 v = _mm256_loadu_ps(src + i);
            v = _mm256_sub_ps(v, vm);
            /* exp via scalar fallback stored to dst, then reload */
            float buf[8];
            _mm256_storeu_ps(buf, v);
            for (int j = 0; j < 8; j++) buf[j] = fast_expf(buf[j]);
            __m256 ev = _mm256_loadu_ps(buf);
            _mm256_storeu_ps(dst + i, ev);
            vsum = _mm256_add_ps(vsum, ev);
        }
        float sum_val = hsum256(vsum);
        for (; i < n; i++) {
            dst[i] = fast_expf(src[i] - max_val);
            sum_val += dst[i];
        }
        if (sum_val == 0.0f) sum_val = 1e-12f;
        __m256 vinv = _mm256_set1_ps(1.0f / sum_val);
        i = 0;
        for (; i + 7 < n; i += 8) {
            __m256 v = _mm256_loadu_ps(dst + i);
            _mm256_storeu_ps(dst + i, _mm256_mul_ps(v, vinv));
        }
        for (; i < n; i++) dst[i] /= sum_val;
        return;
    }
#endif
    for (size_t i = 1; i < n; i++)
        if (src[i] > max_val) max_val = src[i];

    float sum = 0.0f;
    for (size_t i = 0; i < n; i++) {
        dst[i] = fast_expf(src[i] - max_val);
        sum += dst[i];
    }
    if (sum == 0.0f) sum = 1e-12f;
    float inv = 1.0f / sum;
    for (size_t i = 0; i < n; i++) dst[i] *= inv;
}

/* ── SiLU activation: x * sigmoid(x) ────────────────────── */

void simd_silu_f32(float *dst, const float *src, size_t n) {
#if SIMD_AVX2
    if (avx2_available || simd_detect()) {
        size_t i = 0;
        for (; i + 7 < n; i += 8) {
            float buf[8];
            _mm256_storeu_ps(buf, _mm256_loadu_ps(src + i));
            for (int j = 0; j < 8; j++)
                buf[j] = buf[j] / (1.0f + fast_expf(-buf[j]));
            _mm256_storeu_ps(dst + i, _mm256_loadu_ps(buf));
        }
        for (; i < n; i++)
            dst[i] = src[i] / (1.0f + fast_expf(-src[i]));
        return;
    }
#endif
    for (size_t i = 0; i < n; i++)
        dst[i] = src[i] / (1.0f + fast_expf(-src[i]));
}

/* ── RMS Normalization ──────────────────────────────────── */

void simd_rmsnorm_f32(float *dst, const float *src, const float *weight, size_t n) {
    float ss = 0.0f;
#if SIMD_AVX2
    if (avx2_available || simd_detect()) {
        __m256 acc = _mm256_setzero_ps();
        size_t i = 0;
        for (; i + 7 < n; i += 8) {
            __m256 v = _mm256_loadu_ps(src + i);
            acc = _mm256_fmadd_ps(v, v, acc);
        }
        ss = hsum256(acc);
        for (; i < n; i++) ss += src[i] * src[i];

        ss = 1.0f / fast_sqrtf(ss / (float)n + 1e-5f);
        __m256 vs = _mm256_set1_ps(ss);
        i = 0;
        for (; i + 7 < n; i += 8) {
            __m256 v = _mm256_loadu_ps(src + i);
            __m256 w = _mm256_loadu_ps(weight + i);
            _mm256_storeu_ps(dst + i, _mm256_mul_ps(_mm256_mul_ps(v, vs), w));
        }
        for (; i < n; i++) dst[i] = src[i] * ss * weight[i];
        return;
    }
#endif
    for (size_t i = 0; i < n; i++) ss += src[i] * src[i];
    ss = 1.0f / fast_sqrtf(ss / (float)n + 1e-5f);
    for (size_t i = 0; i < n; i++) dst[i] = src[i] * ss * weight[i];
}

/* ── Rotary Position Embedding ─────────────────────────── */

void simd_rope_f32(float *q, float *k, uint32_t n_embd, uint32_t n_head,
                   uint32_t pos, float theta) {
    uint32_t head_dim = n_embd / n_head;
    for (uint32_t h = 0; h < n_head; h++) {
        for (uint32_t i = 0; i < head_dim; i += 2) {
            float freq = 1.0f / fast_powf(theta, (float)i / (float)head_dim);
            float angle = (float)pos * freq;
            float co = fast_cosf(angle);
            float si = fast_sinf(angle);
            uint32_t idx = h * head_dim + i;

            float q0 = q[idx], q1 = q[idx + 1];
            q[idx]     = q0 * co - q1 * si;
            q[idx + 1] = q0 * si + q1 * co;

            if (k) {
                float k0 = k[idx], k1 = k[idx + 1];
                k[idx]     = k0 * co - k1 * si;
                k[idx + 1] = k0 * si + k1 * co;
            }
        }
    }
}
