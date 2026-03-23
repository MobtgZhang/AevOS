#pragma once

#include <aevos/types.h>

struct llm_ctx;

typedef enum {
    LLM_TOOL_ROUTE_HMS,
    LLM_TOOL_ROUTE_AGENT_MAILBOX,
    LLM_TOOL_ROUTE_VFS,
    LLM_TOOL_ROUTE_SKILL_IFC,
    LLM_TOOL_ROUTE_NET_HTTP,
    LLM_TOOL_ROUTE_LOCAL_LLM,
    LLM_TOOL_ROUTE_UNKNOWN
} llm_tool_route_target_t;

typedef struct {
    llm_tool_route_target_t target;
    uint32_t                priority; /* 0 = highest */
    char                    reason[80];
} llm_tool_route_result_t;

/*
 * Decide which L2/L3 subsystem should execute a tool call (name + JSON args).
 * Uses prefix / keyword rules; optional local_ctx reserved for future logits routing.
 */
int llm_tool_route_compute(struct llm_ctx *ctx, const char *tool_name,
                           const char *args_json, llm_tool_route_result_t *out);

/* Compact route line for EventLog / agents: tgt=... pri=... why=... */
int llm_tool_route_format(const llm_tool_route_result_t *res,
                          char *buf, size_t buf_max);
