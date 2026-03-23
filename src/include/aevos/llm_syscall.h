#pragma once

#include <aevos/types.h>

struct llm_ctx;

typedef struct {
    const char *prompt;
    bool        prefer_remote; /* OpenAI-compatible HTTP when net up */
    float       temperature;
    float       top_p;
} llm_infer_request_t;

typedef struct {
    const char *text;
    int        *tokens_out;
    int         max_tokens;
} llm_tokenize_request_t;

typedef struct {
    const char *tool_name;
    const char *args_json;
} llm_tool_dispatch_t;

/* Unified syscall-style entry points for L2 (local GGUF + remote API). */
int llm_sys_infer(struct llm_ctx *local_ctx, const llm_infer_request_t *req,
                  char *output, size_t out_size);
int llm_sys_tokenize(struct llm_ctx *local_ctx, const llm_tokenize_request_t *req);
int llm_sys_tool_dispatch(struct llm_ctx *local_ctx, const llm_tool_dispatch_t *req,
                        char *route_buf, size_t route_max);
