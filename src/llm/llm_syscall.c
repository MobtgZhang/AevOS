#include <aevos/llm_syscall.h>
#include "llm_runtime.h"
#include "llm_api_client.h"
#include "llm_tool_router.h"
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
    if (!llm_is_local_loaded(local_ctx))
        return -ENOTSUP;
    return llm_infer(local_ctx, req->prompt, output, out_size);
}

int llm_sys_tokenize(struct llm_ctx *local_ctx, const llm_tokenize_request_t *req)
{
    if (!local_ctx || !req || !req->text || !req->tokens_out || req->max_tokens <= 0)
        return -EINVAL;
    if (!llm_is_local_loaded(local_ctx))
        return -ENOTSUP;
    return llm_tokenize(local_ctx, req->text, req->tokens_out, req->max_tokens);
}

int llm_sys_tool_dispatch(struct llm_ctx *local_ctx, const llm_tool_dispatch_t *req,
                          char *route_buf, size_t route_max)
{
    if (!req || !req->tool_name || !route_buf || route_max == 0)
        return -EINVAL;

    llm_tool_route_result_t rr;
    int rc = llm_tool_route_compute(local_ctx, req->tool_name,
                                    req->args_json ? req->args_json : "", &rr);
    if (rc < 0)
        return rc;

    char line[192];
    llm_tool_route_format(&rr, line, sizeof(line));

    /* tool=name + routing decision (L2 unified façade) */
    snprintf(route_buf, route_max, "tool=%s %s",
             req->tool_name, line);
    return 0;
}
