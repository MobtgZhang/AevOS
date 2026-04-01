/*
 * AEVOS_EMBED_LLM=0：内核不链接 GGUF/量化内核；推理经 llm_ipc 用户态服务路径。
 */
#include "llm_runtime.h"
#include "kernel/klog.h"
#include "lib/string.h"

static llm_ctx_t *s_llm_singleton;

void llm_kernel_register_singleton(llm_ctx_t *ctx)
{
    s_llm_singleton = ctx;
}

llm_ctx_t *llm_kernel_singleton(void)
{
    return s_llm_singleton;
}

bool llm_is_local_loaded(const llm_ctx_t *ctx)
{
    (void)ctx;
    return false;
}

int llm_init(llm_ctx_t *ctx, const char *model_path)
{
    if (!ctx)
        return -EINVAL;
    memset(ctx, 0, sizeof(*ctx));
    (void)model_path;
    klog("[llm] stub runtime (AEVOS_EMBED_LLM=0): no in-kernel weights\n");
    return 0;
}

void llm_free(llm_ctx_t *ctx)
{
    if (!ctx)
        return;
    memset(ctx, 0, sizeof(*ctx));
}

int llm_reload(llm_ctx_t *ctx, const char *model_path)
{
    (void)ctx;
    (void)model_path;
    return -ENOTSUP;
}

void llm_reset(llm_ctx_t *ctx)
{
    if (ctx)
        ctx->pos = 0;
}

int llm_decode(llm_ctx_t *ctx, int token_id, float *logits)
{
    (void)ctx;
    (void)token_id;
    (void)logits;
    return -ENOTSUP;
}

int llm_generate(llm_ctx_t *ctx, const int *input_ids, int n_input,
                 int max_new_tokens, token_cb_t cb, void *userdata)
{
    (void)ctx;
    (void)input_ids;
    (void)n_input;
    (void)max_new_tokens;
    (void)cb;
    (void)userdata;
    return -ENOTSUP;
}

int llm_tokenize(llm_ctx_t *ctx, const char *text, int *tokens, int max_tokens)
{
    (void)ctx;
    (void)text;
    (void)tokens;
    (void)max_tokens;
    return -ENOTSUP;
}

const char *llm_detokenize(llm_ctx_t *ctx, int token)
{
    (void)ctx;
    (void)token;
    return "";
}

int llm_sample(const float *logits, uint32_t n_vocab,
               float temperature, float top_p)
{
    (void)logits;
    (void)n_vocab;
    (void)temperature;
    (void)top_p;
    return -1;
}

int llm_infer(llm_ctx_t *ctx, const char *prompt, char *output, size_t out_size)
{
    (void)ctx;
    (void)prompt;
    if (output && out_size)
        output[0] = '\0';
    return -ENOTSUP;
}
