#include "cap_ipc.h"
#include "../klog.h"
#include "lib/string.h"

static cap_ipc_endpoint_t g_eps[CAP_IPC_MAX_ENDPOINTS];
static uint64_t           g_next_tok = 1;

void cap_ipc_init(void)
{
    memset(g_eps, 0, sizeof(g_eps));
    klog("cap_ipc: %u endpoint slots (capability routing scaffold)\n",
         (unsigned)CAP_IPC_MAX_ENDPOINTS);
}

uint64_t cap_ipc_grant(cap_ipc_endpoint_class_t ep, uint32_t owner_domain)
{
    for (int i = 0; i < CAP_IPC_MAX_ENDPOINTS; i++) {
        if (g_eps[i].magic == 0) {
            g_eps[i].magic        = CAP_IPC_MAGIC;
            g_eps[i].token        = g_next_tok++;
            g_eps[i].ep_class     = ep;
            g_eps[i].owner_domain = owner_domain;
            g_eps[i].revoked      = false;
            return g_eps[i].token;
        }
    }
    return 0;
}

bool cap_ipc_validate(uint64_t token, cap_ipc_endpoint_class_t expected_ep)
{
    if (token == 0)
        return false;
    for (int i = 0; i < CAP_IPC_MAX_ENDPOINTS; i++) {
        if (g_eps[i].magic == CAP_IPC_MAGIC && g_eps[i].token == token
            && !g_eps[i].revoked && g_eps[i].ep_class == expected_ep)
            return true;
    }
    return false;
}

void cap_ipc_revoke(uint64_t token)
{
    for (int i = 0; i < CAP_IPC_MAX_ENDPOINTS; i++) {
        if (g_eps[i].token == token) {
            g_eps[i].revoked = true;
            return;
        }
    }
}

int cap_ipc_deliver(uint64_t token, const cap_ipc_message_t *msg)
{
    if (!msg)
        return -EINVAL;
    for (int i = 0; i < CAP_IPC_MAX_ENDPOINTS; i++) {
        if (g_eps[i].magic != CAP_IPC_MAGIC || g_eps[i].revoked
            || g_eps[i].token != token)
            continue;
        (void)msg->msg_type;
        (void)g_eps[i].ep_class;
        return 0;
    }
    return -EPERM;
}
