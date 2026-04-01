#pragma once

#include <aevos/types.h>

/*
 * 能力型 IPC（content1）：内核校验 capability token，将消息路由到用户态服务。
 * 与 LC IFC 共用「域」思想；令牌为不透明 64 位句柄。
 */

#define CAP_IPC_MAX_ENDPOINTS 32
#define CAP_IPC_MAGIC         0xCA704970ULL

typedef enum {
    CAP_IPC_EP_VFS = 1,
    CAP_IPC_EP_NET,
    CAP_IPC_EP_SCHED,
    CAP_IPC_EP_GPU,
    CAP_IPC_EP_LLM,
    CAP_IPC_EP_LINUX_SHIM,
} cap_ipc_endpoint_class_t;

typedef struct {
    uint64_t                   magic;
    uint64_t                   token;
    cap_ipc_endpoint_class_t   ep_class;
    uint32_t                   owner_domain; /* lc_ifc_domain_t 数值 */
    bool                       revoked;
} cap_ipc_endpoint_t;

typedef struct {
    uint32_t msg_type;
    uint32_t payload_len;
    uint8_t  payload[256];
} cap_ipc_message_t;

void cap_ipc_init(void);

/* 颁发令牌（简化：单调计数 + ep_class）；失败返回 0。 */
uint64_t cap_ipc_grant(cap_ipc_endpoint_class_t ep, uint32_t owner_domain);

bool cap_ipc_validate(uint64_t token, cap_ipc_endpoint_class_t expected_ep);

void cap_ipc_revoke(uint64_t token);

/*
 * 路由占位：校验后将消息复制到服务端环形区（未来与用户态共享页）。
 * 返回 0 或负 errno。
 */
int cap_ipc_deliver(uint64_t token, const cap_ipc_message_t *msg);
