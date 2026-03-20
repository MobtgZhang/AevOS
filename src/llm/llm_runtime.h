#pragma once

#include <aevos/types.h>
#include <aevos/config.h>

typedef struct {
    uint32_t n_vocab;
    uint32_t n_ctx;
    uint32_t n_embd;
    uint32_t n_layer;
    uint32_t n_head;
    uint32_t n_head_kv;
    uint32_t n_ff;
    float    rope_theta;
    bool     use_avx2;
} llm_config_t;

typedef struct llm_weights {
    void  *token_embd;
    void **wq;
    void **wk;
    void **wv;
    void **wo;
    void **w_gate;
    void **w_up;
    void **w_down;
    float **rms_att;
    float **rms_ffn;
    float  *rms_final;
    void   *output_weight;
    uint8_t quant_type;
} llm_weights_t;

typedef struct llm_ctx {
    llm_config_t   config;
    float         *k_cache;
    float         *v_cache;
    llm_weights_t  weights;
    size_t         weights_size;
    uint32_t       pos;
    bool           has_new_token;
    char           last_token_text[256];

    char         **vocab;
    float         *vocab_scores;
    uint32_t       vocab_size;

    float         *logits;
    float         *embd;
    float         *work_buf;
    size_t         work_buf_size;
} llm_ctx_t;

typedef void (*token_cb_t)(int token, const char *text, void *userdata);

int  llm_init(llm_ctx_t *ctx, const char *model_path);
void llm_free(llm_ctx_t *ctx);
void llm_reset(llm_ctx_t *ctx);

int llm_decode(llm_ctx_t *ctx, int token_id, float *logits);
int llm_generate(llm_ctx_t *ctx, const int *input_ids, int n_input,
                 int max_new_tokens, token_cb_t cb, void *userdata);

int         llm_tokenize(llm_ctx_t *ctx, const char *text, int *tokens, int max_tokens);
const char *llm_detokenize(llm_ctx_t *ctx, int token);

int llm_sample(const float *logits, uint32_t n_vocab,
               float temperature, float top_p);

int llm_infer(llm_ctx_t *ctx, const char *prompt, char *output, size_t out_size);
