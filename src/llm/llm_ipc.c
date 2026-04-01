#include <aevos/llm_ipc.h>
#include <aevos/llm_syscall.h>
#include "kernel/klog.h"
#include "lib/string.h"

static bool     s_server_present;
static uint64_t s_next_seq = 1;

static struct {
    llm_ipc_infer_slot_t slots[LLM_IPC_RING_CAP];
    uint32_t             head;
    uint32_t             tail;
    uint32_t             count;
} s_q;

static struct {
    uint64_t seq;
    char     text[768];
    size_t   len;
    bool     valid;
} s_resp;

void llm_ipc_init(void)
{
    memset(&s_q, 0, sizeof(s_q));
    s_server_present = false;
    s_resp.valid     = false;
    klog("llm_ipc: ring capacity %u (userspace server path)\n",
         (unsigned)LLM_IPC_RING_CAP);
}

void llm_ipc_set_userspace_server_present(bool present)
{
    s_server_present = present;
}

bool llm_ipc_userspace_server_present(void)
{
    return s_server_present;
}

int llm_ipc_try_infer(const llm_infer_request_t *req, char *out, size_t out_max)
{
    if (!req || !req->prompt || !out || out_max == 0)
        return -EINVAL;
    if (!s_server_present)
        return -ENOTSUP;
    if (s_q.count >= LLM_IPC_RING_CAP)
        return -EAGAIN;

    llm_ipc_infer_slot_t *sl = &s_q.slots[s_q.tail];
    sl->seq         = s_next_seq++;
    snprintf(sl->prompt, sizeof(sl->prompt), "%s", req->prompt);
    sl->temperature = req->temperature;
    sl->top_p       = req->top_p;
    s_q.tail        = (s_q.tail + 1u) % LLM_IPC_RING_CAP;
    s_q.count++;

    klog("llm_ipc: infer req seq=%llu queued (async reply not wired)\n",
         (unsigned long long)sl->seq);
    (void)out_max;
    return -EAGAIN;
}

bool llm_ipc_server_dequeue_infer(llm_ipc_infer_slot_t *slot)
{
    if (!slot || s_q.count == 0)
        return false;
    *slot = s_q.slots[s_q.head];
    s_q.head = (s_q.head + 1u) % LLM_IPC_RING_CAP;
    s_q.count--;
    return true;
}

void llm_ipc_server_post_response(uint64_t seq, const char *text, size_t len)
{
    s_resp.seq = seq;
    if (len >= sizeof(s_resp.text))
        len = sizeof(s_resp.text) - 1;
    memcpy(s_resp.text, text, len);
    s_resp.text[len] = '\0';
    s_resp.len       = len;
    s_resp.valid     = true;
}

int llm_ipc_take_response(uint64_t seq, char *out, size_t out_max)
{
    if (!out || out_max == 0 || !s_resp.valid || s_resp.seq != seq)
        return -ENOENT;
    size_t n = s_resp.len;
    if (n >= out_max)
        n = out_max - 1;
    memcpy(out, s_resp.text, n);
    out[n]         = '\0';
    s_resp.valid   = false;
    return 0;
}
