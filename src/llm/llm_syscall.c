#include <aevos/llm_syscall.h>
#include "llm_runtime.h"
#include "llm_api_client.h"
#include "lib/string.h"

int llm_sys_infer(struct llm_ctx *local_ctx, const llm_infer_request_t *req,
                  char *output, size_t out_size)
{
    if (!req || !output || out_size == 0)
        return -EINVAL;

    if (req->prefer_remote) {
        int r = llm_remote_infer(req, output, out_size);
        if (r == 0 || r != -ENOTSUP)
            return r;
    }

    if (!local_ctx || !req->prompt)
        return -EINVAL;
    return llm_infer(local_ctx, req->prompt, output, out_size);
}

int llm_sys_tokenize(struct llm_ctx *local_ctx, const llm_tokenize_request_t *req)
{
    if (!local_ctx || !req || !req->text || !req->tokens_out || req->max_tokens <= 0)
        return -EINVAL;
    return llm_tokenize(local_ctx, req->text, req->tokens_out, req->max_tokens);
}

int llm_sys_tool_dispatch(struct llm_ctx *local_ctx, const llm_tool_dispatch_t *req,
                          char *route_buf, size_t route_max)
{
    (void)local_ctx;
    if (!req || !req->tool_name || !route_buf || route_max == 0)
        return -EINVAL;
    snprintf(route_buf, route_max, "tool:%s:%s",
             req->tool_name, req->args_json ? req->args_json : "");
    return 0;
}
