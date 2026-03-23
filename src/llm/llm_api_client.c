#include "llm_api_client.h"
#include "kernel/klog.h"
#include "lib/string.h"

int llm_remote_infer(const llm_infer_request_t *req, char *out, size_t out_max)
{
    (void)req;
    if (out && out_max)
        out[0] = '\0';
    klog("llm_remote: OpenAI-compatible path not yet wired (need TCP+HTTP)\n");
    return -ENOTSUP;
}
