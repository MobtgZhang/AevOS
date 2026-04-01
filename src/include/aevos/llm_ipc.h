#pragma once

#include <aevos/types.h>
#include <aevos/llm_syscall.h>

/*
 * Cap-IPC 边界（content1 P0）：本地 GGUF 推理迁出内核后，内核经此队列把请求交给
 * 用户态 LLM 服务进程。当前为内核内环形占位，供 AEVOS_EMBED_LLM=0 路径使用。
 */

#define LLM_IPC_RING_CAP 8

typedef struct {
    uint64_t seq;
    char     prompt[512];
    float    temperature;
    float    top_p;
} llm_ipc_infer_slot_t;

void llm_ipc_init(void);

/* 用户态服务就绪后应调用（未来由 IPC 注册替代）。 */
void llm_ipc_set_userspace_server_present(bool present);

bool llm_ipc_userspace_server_present(void);

/*
 * 非嵌入模式：尝试经 IPC 完成推理。成功写 out 并返回 0；无服务返回 -ENOTSUP；
 * 队列满返回 -EAGAIN。
 */
int llm_ipc_try_infer(const llm_infer_request_t *req,
                      char *out, size_t out_max);

/* 由未来「LLM 服务」协程/进程调用：取出待处理槽（无则 false）。 */
bool llm_ipc_server_dequeue_infer(llm_ipc_infer_slot_t *slot);

/* 服务写回结果（简化：单槽响应）。 */
void llm_ipc_server_post_response(uint64_t seq, const char *text, size_t len);

int llm_ipc_take_response(uint64_t seq, char *out, size_t out_max);
