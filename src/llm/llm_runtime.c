#include "llm_runtime.h"
#include "gguf_loader.h"
#include "safetensors_loader.h"
#include "quantize.h"
#include "simd_kernels.h"
#include "kernel/mm/slab.h"
#include "kernel/klog.h"
#include "lib/string.h"

/* ── Fast math (duplicated locally for inlining) ──────────── */

static float rt_expf(float x) {
    if (x > 88.72f)  return 3.4028235e+38f;
    if (x < -87.33f) return 0.0f;
    float kf = x * 1.4426950408889634f;
    int32_t k = (int32_t)(kf + (kf >= 0 ? 0.5f : -0.5f));
    float r  = x - (float)k * 0.6931471805599453f;
    float r2 = r * r;
    float p  = 1.0f + r + r2 * 0.5f + r2 * r * (1.0f / 6.0f)
             + r2 * r2 * (1.0f / 24.0f);
    union { float f; int32_t i; } u = { p };
    u.i += k << 23;
    return u.f;
}

/* ── Simple xorshift64 PRNG for sampling ─────────────────── */

static uint64_t rng_state = 0xDEADBEEFCAFEBABEULL;

static uint64_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static float rng_uniform(void) {
    return (float)(rng_next() & 0xFFFFFFu) / (float)0xFFFFFFu;
}

/* ── Quantized mat-vec: out[n_out] = W[n_out,n_in] · x[n_in] ─ */

static void qmatvec(float *out, const void *W, const float *x,
                     uint32_t n_out, uint32_t n_in, uint8_t qtype) {
    if (qtype == GGUF_TYPE_Q4_0) {
        size_t blocks_per_row = n_in / QK4_0;
        /* Quantize x into Q8_0 scratch */
        size_t q8_count = n_in / QK8_0;
        q8_0_block_t *xq = (q8_0_block_t *)kmalloc(q8_count * sizeof(q8_0_block_t));
        if (!xq) return;
        quantize_f32_to_q8_0(x, xq, n_in);

        const q4_0_block_t *Wq = (const q4_0_block_t *)W;
        for (uint32_t r = 0; r < n_out; r++)
            out[r] = vec_dot_q4_0_q8_0(Wq + r * blocks_per_row, xq, n_in);

        kfree(xq);
    } else if (qtype == GGUF_TYPE_Q8_0) {
        size_t blocks_per_row = n_in / QK8_0;
        q8_0_block_t *xq = (q8_0_block_t *)kmalloc(blocks_per_row * sizeof(q8_0_block_t));
        if (!xq) return;
        quantize_f32_to_q8_0(x, xq, n_in);

        const q8_0_block_t *Wq = (const q8_0_block_t *)W;
        for (uint32_t r = 0; r < n_out; r++) {
            float s = 0.0f;
            for (size_t b = 0; b < blocks_per_row; b++) {
                float dw = f16_to_f32(Wq[r * blocks_per_row + b].delta);
                float dx = f16_to_f32(xq[b].delta);
                int32_t dot = 0;
                for (int i = 0; i < QK8_0; i++)
                    dot += (int)Wq[r * blocks_per_row + b].quants[i] * (int)xq[b].quants[i];
                s += dw * dx * (float)dot;
            }
            out[r] = s;
        }
        kfree(xq);
    } else {
        /* F32 fallback */
        const float *Wf = (const float *)W;
        for (uint32_t r = 0; r < n_out; r++)
            out[r] = simd_vec_dot_f32(Wf + (size_t)r * n_in, x, n_in);
    }
}

/* ── llm_init ─────────────────────────────────────────────── */

int llm_init(llm_ctx_t *ctx, const char *model_path) {
    if (!ctx || !model_path) return -EINVAL;
    memset(ctx, 0, sizeof(*ctx));

    simd_detect();
    ctx->config.use_avx2 = simd_detect();

    /* Try GGUF format first, then SafeTensors */
    gguf_file_t *gf = gguf_open(model_path);
    if (gf) {
        int rc = gguf_load_into_ctx(gf, ctx);
        if (rc < 0) {
            gguf_close(gf);
            return rc;
        }
    }

    if (!gf) {
        st_file_t *sf = st_open(model_path);
        if (sf) {
            int rc = st_load_into_ctx(sf, ctx);
            if (rc < 0) {
                st_close(sf);
                return rc;
            }
        }
    }

    if (!gf) {
        /* Minimal default config for testing without a model file */
        ctx->config.n_vocab   = 32000;
        ctx->config.n_ctx     = LLM_DEFAULT_CTX;
        ctx->config.n_embd    = 4096;
        ctx->config.n_layer   = 32;
        ctx->config.n_head    = 32;
        ctx->config.n_head_kv = 32;
        ctx->config.n_ff      = 11008;
        ctx->config.rope_theta = 10000.0f;

        uint32_t kv_dim = ctx->config.n_embd;
        size_t csz = (size_t)ctx->config.n_layer * ctx->config.n_ctx
                   * kv_dim * sizeof(float);
        ctx->k_cache = (float *)kcalloc(1, csz);
        ctx->v_cache = (float *)kcalloc(1, csz);
        ctx->logits  = (float *)kmalloc(ctx->config.n_vocab * sizeof(float));
        ctx->embd    = (float *)kmalloc(ctx->config.n_embd  * sizeof(float));

        size_t work_need = (size_t)ctx->config.n_embd * 5
                         + (size_t)ctx->config.n_head * ctx->config.n_ctx
                         + (size_t)ctx->config.n_ff * 2
                         + (size_t)ctx->config.n_vocab;
        ctx->work_buf_size = work_need * sizeof(float);
        ctx->work_buf = (float *)kmalloc(ctx->work_buf_size);
    }

    ctx->pos = 0;
    ctx->has_new_token = false;
    klog("llm: initialized (avx2=%d)\n", ctx->config.use_avx2);
    return 0;
}

/* ── llm_free ─────────────────────────────────────────────── */

void llm_free(llm_ctx_t *ctx) {
    if (!ctx) return;
    kfree(ctx->k_cache);
    kfree(ctx->v_cache);
    kfree(ctx->logits);
    kfree(ctx->embd);
    kfree(ctx->work_buf);

    if (ctx->weights.wq)      kfree(ctx->weights.wq);
    if (ctx->weights.wk)      kfree(ctx->weights.wk);
    if (ctx->weights.wv)      kfree(ctx->weights.wv);
    if (ctx->weights.wo)      kfree(ctx->weights.wo);
    if (ctx->weights.w_gate)  kfree(ctx->weights.w_gate);
    if (ctx->weights.w_up)    kfree(ctx->weights.w_up);
    if (ctx->weights.w_down)  kfree(ctx->weights.w_down);
    if (ctx->weights.rms_att) kfree(ctx->weights.rms_att);
    if (ctx->weights.rms_ffn) kfree(ctx->weights.rms_ffn);

    if (ctx->vocab) {
        for (uint32_t i = 0; i < ctx->vocab_size; i++)
            kfree(ctx->vocab[i]);
        kfree(ctx->vocab);
    }

    memset(ctx, 0, sizeof(*ctx));
}

/* ── llm_reset ────────────────────────────────────────────── */

void llm_reset(llm_ctx_t *ctx) {
    if (!ctx) return;
    ctx->pos = 0;
    ctx->has_new_token = false;
    uint32_t kv_dim = (ctx->config.n_embd / ctx->config.n_head) * ctx->config.n_head_kv;
    size_t csz = (size_t)ctx->config.n_layer * ctx->config.n_ctx * kv_dim * sizeof(float);
    if (ctx->k_cache) memset(ctx->k_cache, 0, csz);
    if (ctx->v_cache) memset(ctx->v_cache, 0, csz);
}

/* ── llm_decode — single token forward pass ──────────────── */

int llm_decode(llm_ctx_t *ctx, int token_id, float *out_logits) {
    if (!ctx) return -EINVAL;

    llm_config_t *cfg = &ctx->config;
    uint32_t n_embd    = cfg->n_embd;
    uint32_t n_layer   = cfg->n_layer;
    uint32_t n_head    = cfg->n_head;
    uint32_t n_head_kv = cfg->n_head_kv;
    uint32_t head_dim  = n_embd / n_head;
    uint32_t kv_dim    = head_dim * n_head_kv;
    uint32_t pos       = ctx->pos;
    uint8_t  qt        = ctx->weights.quant_type;
    llm_weights_t *w   = &ctx->weights;

    /* Carve working buffer into named regions */
    float *cur   = ctx->work_buf;
    float *q_buf = cur;              cur += n_embd;
    float *k_buf = cur;              cur += kv_dim;
    float *v_buf = cur;              cur += kv_dim;
    float *att   = cur;              cur += n_head * cfg->n_ctx;
    float *ffn1  = cur;              cur += cfg->n_ff;
    float *ffn2  = cur;              cur += cfg->n_ff;
    float *norm  = cur;              cur += n_embd;

    float *x = ctx->embd;

    /* Token embedding lookup */
    if (w->token_embd) {
        if (qt == GGUF_TYPE_F32 || qt == 0) {
            const float *emb_table = (const float *)w->token_embd;
            memcpy(x, emb_table + (size_t)token_id * n_embd, n_embd * sizeof(float));
        } else {
            /* Quantized embedding: dequantize the row */
            const q4_0_block_t *emb_q = (const q4_0_block_t *)w->token_embd;
            size_t blocks_per_row = n_embd / QK4_0;
            dequantize_q4_0_to_f32(emb_q + (size_t)token_id * blocks_per_row, x, n_embd);
        }
    } else {
        memset(x, 0, n_embd * sizeof(float));
    }

    /* Transformer layers */
    for (uint32_t l = 0; l < n_layer; l++) {
        size_t kv_off = (size_t)l * cfg->n_ctx * kv_dim;

        /* ── Attention pre-norm (RMSNorm) ── */
        if (w->rms_att && w->rms_att[l])
            simd_rmsnorm_f32(norm, x, w->rms_att[l], n_embd);
        else
            memcpy(norm, x, n_embd * sizeof(float));

        /* ── Q/K/V projections ── */
        if (w->wq && w->wq[l])
            qmatvec(q_buf, w->wq[l], norm, n_embd, n_embd, qt);
        else
            memcpy(q_buf, norm, n_embd * sizeof(float));

        if (w->wk && w->wk[l])
            qmatvec(k_buf, w->wk[l], norm, kv_dim, n_embd, qt);
        else
            memcpy(k_buf, norm, kv_dim * sizeof(float));

        if (w->wv && w->wv[l])
            qmatvec(v_buf, w->wv[l], norm, kv_dim, n_embd, qt);
        else
            memcpy(v_buf, norm, kv_dim * sizeof(float));

        /* ── RoPE on Q and K ── */
        simd_rope_f32(q_buf, k_buf, n_embd, n_head, pos, cfg->rope_theta);

        /* ── Store K, V into cache ── */
        memcpy(ctx->k_cache + kv_off + (size_t)pos * kv_dim,
               k_buf, kv_dim * sizeof(float));
        memcpy(ctx->v_cache + kv_off + (size_t)pos * kv_dim,
               v_buf, kv_dim * sizeof(float));

        /* ── Multi-head attention ── */
        uint32_t kv_groups = n_head / n_head_kv;

        for (uint32_t h = 0; h < n_head; h++) {
            uint32_t kv_h = h / kv_groups;
            float *q_head = q_buf + h * head_dim;

            /* Compute attention scores for all positions up to pos */
            for (uint32_t t = 0; t <= pos; t++) {
                float *k_t = ctx->k_cache + kv_off + (size_t)t * kv_dim + kv_h * head_dim;
                float score = simd_vec_dot_f32(q_head, k_t, head_dim);
                float scale = 1.0f;
                /* fast_sqrtf inline */
                {
                    union { float f; uint32_t i; } su = { (float)head_dim };
                    su.i = (su.i >> 1) + 0x1FC00000u;
                    su.f = 0.5f * (su.f + (float)head_dim / su.f);
                    scale = 1.0f / su.f;
                }
                att[h * cfg->n_ctx + t] = score * scale;
            }

            /* Softmax over [0..pos] */
            simd_softmax_f32(att + h * cfg->n_ctx,
                             att + h * cfg->n_ctx, pos + 1);

            /* Weighted sum of V */
            float *out_head = norm + h * head_dim;
            memset(out_head, 0, head_dim * sizeof(float));
            for (uint32_t t = 0; t <= pos; t++) {
                float a = att[h * cfg->n_ctx + t];
                float *v_t = ctx->v_cache + kv_off + (size_t)t * kv_dim + kv_h * head_dim;
                for (uint32_t d = 0; d < head_dim; d++)
                    out_head[d] += a * v_t[d];
            }
        }

        /* ── Output projection ── */
        if (w->wo && w->wo[l]) {
            qmatvec(q_buf, w->wo[l], norm, n_embd, n_embd, qt);
        } else {
            memcpy(q_buf, norm, n_embd * sizeof(float));
        }

        /* Residual */
        simd_vec_add_f32(x, x, q_buf, n_embd);

        /* ── FFN pre-norm ── */
        if (w->rms_ffn && w->rms_ffn[l])
            simd_rmsnorm_f32(norm, x, w->rms_ffn[l], n_embd);
        else
            memcpy(norm, x, n_embd * sizeof(float));

        /* ── Feed-Forward Network (SwiGLU) ── */
        if (w->w_gate && w->w_gate[l])
            qmatvec(ffn1, w->w_gate[l], norm, cfg->n_ff, n_embd, qt);
        else
            memset(ffn1, 0, cfg->n_ff * sizeof(float));

        if (w->w_up && w->w_up[l])
            qmatvec(ffn2, w->w_up[l], norm, cfg->n_ff, n_embd, qt);
        else
            memset(ffn2, 0, cfg->n_ff * sizeof(float));

        /* SiLU(gate) * up */
        simd_silu_f32(ffn1, ffn1, cfg->n_ff);
        simd_vec_mul_f32(ffn1, ffn1, ffn2, cfg->n_ff);

        /* Down projection */
        if (w->w_down && w->w_down[l])
            qmatvec(norm, w->w_down[l], ffn1, n_embd, cfg->n_ff, qt);
        else
            memset(norm, 0, n_embd * sizeof(float));

        /* Residual */
        simd_vec_add_f32(x, x, norm, n_embd);
    }

    /* ── Final RMS norm ── */
    if (w->rms_final)
        simd_rmsnorm_f32(x, x, w->rms_final, n_embd);

    /* ── Output logits ── */
    float *target = out_logits ? out_logits : ctx->logits;
    if (w->output_weight)
        qmatvec(target, w->output_weight, x, cfg->n_vocab, n_embd, qt);
    else
        memset(target, 0, cfg->n_vocab * sizeof(float));

    ctx->pos++;
    return 0;
}

/* ── Sampling ────────────────────────────────────────────── */

int llm_sample(const float *logits, uint32_t n_vocab,
               float temperature, float top_p) {
    if (!logits || n_vocab == 0) return 0;

    /* Greedy if temperature ≈ 0 */
    if (temperature < 1e-6f) {
        int best = 0;
        float best_v = logits[0];
        for (uint32_t i = 1; i < n_vocab; i++) {
            if (logits[i] > best_v) { best_v = logits[i]; best = (int)i; }
        }
        return best;
    }

    /* Apply temperature */
    float *probs = (float *)kmalloc(n_vocab * sizeof(float));
    if (!probs) return 0;

    float max_l = logits[0];
    for (uint32_t i = 1; i < n_vocab; i++)
        if (logits[i] > max_l) max_l = logits[i];

    float sum = 0.0f;
    for (uint32_t i = 0; i < n_vocab; i++) {
        probs[i] = rt_expf((logits[i] - max_l) / temperature);
        sum += probs[i];
    }
    if (sum > 0.0f) {
        float inv = 1.0f / sum;
        for (uint32_t i = 0; i < n_vocab; i++) probs[i] *= inv;
    }

    /* Top-p (nucleus) sampling: sort by probability descending */
    /* Simple selection sort on indices (n_vocab can be large, but this
     * only needs to process until cumulative >= top_p) */
    float cumul = 0.0f;
    int   token = 0;

    /* Partial argmax loop for top-p */
    bool *used = (bool *)kcalloc(n_vocab, sizeof(bool));
    if (!used) { kfree(probs); return 0; }

    float threshold = rng_uniform();
    float running = 0.0f;

    while (cumul < top_p) {
        /* Find highest remaining probability */
        int best = -1;
        float best_p = -1.0f;
        for (uint32_t i = 0; i < n_vocab; i++) {
            if (!used[i] && probs[i] > best_p) {
                best_p = probs[i]; best = (int)i;
            }
        }
        if (best < 0) break;
        used[best] = true;
        cumul += best_p;

        running += best_p;
        if (running >= threshold * cumul) {
            token = best;
            break;
        }
        token = best;
    }

    kfree(used);
    kfree(probs);
    return token;
}

/* ── Tokenizer (byte-level BPE) ──────────────────────────── */

int llm_tokenize(llm_ctx_t *ctx, const char *text, int *tokens, int max_tokens) {
    if (!ctx || !text || !tokens) return -EINVAL;

    size_t len = strlen(text);
    if (len == 0) return 0;

    /* Start with byte-level tokens (each byte → token ID).
     * If vocab is available, bytes map to IDs 0..255.
     * Then iteratively merge the best pair using vocab_scores. */

    int n = 0;
    for (size_t i = 0; i < len && n < max_tokens; i++)
        tokens[n++] = (unsigned char)text[i];

    if (!ctx->vocab || !ctx->vocab_scores || ctx->vocab_size <= 256)
        return n;

    /* Iterative BPE merge */
    while (n >= 2) {
        float best_score = -1e30f;
        int   best_pos   = -1;
        int   best_id    = -1;

        for (int i = 0; i < n - 1; i++) {
            /* Build candidate merged string */
            const char *s1 = llm_detokenize(ctx, tokens[i]);
            const char *s2 = llm_detokenize(ctx, tokens[i + 1]);
            if (!s1 || !s2) continue;

            char merged[512];
            size_t l1 = strlen(s1), l2 = strlen(s2);
            if (l1 + l2 >= sizeof(merged)) continue;
            memcpy(merged, s1, l1);
            memcpy(merged + l1, s2, l2);
            merged[l1 + l2] = '\0';

            /* Search vocabulary for merged token */
            for (uint32_t v = 256; v < ctx->vocab_size; v++) {
                if (ctx->vocab[v] && strcmp(ctx->vocab[v], merged) == 0) {
                    if (ctx->vocab_scores[v] > best_score) {
                        best_score = ctx->vocab_scores[v];
                        best_pos = i;
                        best_id = (int)v;
                    }
                    break;
                }
            }
        }

        if (best_pos < 0) break;

        tokens[best_pos] = best_id;
        /* Shift remaining tokens left by one */
        for (int i = best_pos + 1; i < n - 1; i++)
            tokens[i] = tokens[i + 1];
        n--;
    }

    return n;
}

/* ── Detokenize ──────────────────────────────────────────── */

const char *llm_detokenize(llm_ctx_t *ctx, int token) {
    if (!ctx) return NULL;

    /* Byte-level fallback: tokens 0-255 are individual bytes */
    if (token >= 0 && token < 256) {
        static char byte_strs[256][2];
        byte_strs[token][0] = (char)token;
        byte_strs[token][1] = '\0';
        return byte_strs[token];
    }

    if (ctx->vocab && (uint32_t)token < ctx->vocab_size && ctx->vocab[token])
        return ctx->vocab[token];

    return "";
}

/* ── Auto-regressive generation ──────────────────────────── */

int llm_generate(llm_ctx_t *ctx, const int *input_ids, int n_input,
                 int max_new_tokens, token_cb_t cb, void *userdata) {
    if (!ctx || !input_ids) return -EINVAL;

    /* Prefill: process all input tokens */
    for (int i = 0; i < n_input; i++) {
        int rc = llm_decode(ctx, input_ids[i], NULL);
        if (rc < 0) return rc;
    }

    /* Generate new tokens */
    int generated = 0;
    for (int t = 0; t < max_new_tokens; t++) {
        int tok = llm_sample(ctx->logits, ctx->config.n_vocab, 0.7f, 0.9f);

        /* EOS check (token 2 is common EOS) */
        if (tok == 2) break;

        /* Decode the new token */
        int rc = llm_decode(ctx, tok, NULL);
        if (rc < 0) return rc;

        const char *text = llm_detokenize(ctx, tok);
        if (text) {
            strncpy(ctx->last_token_text, text, sizeof(ctx->last_token_text) - 1);
            ctx->last_token_text[sizeof(ctx->last_token_text) - 1] = '\0';
        }
        ctx->has_new_token = true;

        if (cb) cb(tok, text, userdata);
        generated++;

        if (ctx->pos >= ctx->config.n_ctx) break;
    }

    return generated;
}

/* ── High-level inference: prompt → response string ─────── */

typedef struct {
    char   *buf;
    size_t  size;
    size_t  pos;
} infer_state_t;

static void infer_cb(int token, const char *text, void *ud) {
    (void)token;
    infer_state_t *st = (infer_state_t *)ud;
    if (!text) return;
    size_t tl = strlen(text);
    if (st->pos + tl < st->size) {
        memcpy(st->buf + st->pos, text, tl);
        st->pos += tl;
        st->buf[st->pos] = '\0';
    }
}

int llm_infer(llm_ctx_t *ctx, const char *prompt, char *output, size_t out_size) {
    if (!ctx || !prompt || !output || out_size == 0) return -EINVAL;

    output[0] = '\0';

    int *tokens = (int *)kmalloc(ctx->config.n_ctx * sizeof(int));
    if (!tokens) return -ENOMEM;

    int n_tok = llm_tokenize(ctx, prompt, tokens, (int)ctx->config.n_ctx);
    if (n_tok < 0) { kfree(tokens); return n_tok; }

    llm_reset(ctx);

    infer_state_t st = { output, out_size, 0 };
    int max_gen = (int)(ctx->config.n_ctx - (uint32_t)n_tok);
    if (max_gen > LLM_MAX_BATCH) max_gen = LLM_MAX_BATCH;
    if (max_gen < 1) max_gen = 1;

    int gen = llm_generate(ctx, tokens, n_tok, max_gen, infer_cb, &st);

    kfree(tokens);
    return gen;
}
