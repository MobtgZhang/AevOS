#include "llm_tool_router.h"
#include "llm_runtime.h"
#include "lib/string.h"

static bool starts_with(const char *s, const char *p) {
    if (!s || !p) return false;
    while (*p) {
        if (*s++ != *p++) return false;
    }
    return true;
}

static bool name_has_any(const char *name, const char *const *keys) {
    if (!name) return false;
    for (; *keys; keys++) {
        if (strstr(name, *keys))
            return true;
    }
    return false;
}

static void args_hint(const char *args, llm_tool_route_result_t *out) {
    if (!args || !out)
        return;
    if (strstr(args, "\"path\"") || strstr(args, "filepath") || strstr(args, "/")) {
        out->target   = LLM_TOOL_ROUTE_VFS;
        out->priority = 1;
        strncpy(out->reason, "args_path", sizeof(out->reason) - 1);
        out->reason[sizeof(out->reason) - 1] = '\0';
        return;
    }
    if (strstr(args, "http://") || strstr(args, "https://") || strstr(args, "\"url\"")) {
        out->target   = LLM_TOOL_ROUTE_NET_HTTP;
        out->priority = 1;
        strncpy(out->reason, "args_url", sizeof(out->reason) - 1);
        out->reason[sizeof(out->reason) - 1] = '\0';
    }
}

int llm_tool_route_compute(struct llm_ctx *ctx, const char *tool_name,
                           const char *args_json, llm_tool_route_result_t *out) {
    (void)ctx;

    if (!tool_name || !out)
        return -EINVAL;

    memset(out, 0, sizeof(*out));
    out->target   = LLM_TOOL_ROUTE_UNKNOWN;
    out->priority = 8;
    strncpy(out->reason, "default", sizeof(out->reason) - 1);

    static const char *vfs_keys[] = {
        "read_file", "write_file", "list_dir", "fs_", "file_", "path_", NULL
    };
    static const char *hms_keys[] = {
        "memory_", "hms_", "vector_", "recall_", "embed_", "history_search", NULL
    };
    static const char *mb_keys[] = {
        "mailbox_", "agent_send", "broadcast_", "notify_", NULL
    };
    static const char *skill_keys[] = {
        "skill_", "sandbox_", "exec_skill", "run_elf", "ifc_", NULL
    };
    static const char *net_keys[] = {
        "http_", "fetch_", "openai_", "remote_", "api_request", NULL
    };

    if (starts_with(tool_name, "functions."))
        tool_name += 10;

    if (name_has_any(tool_name, vfs_keys)) {
        out->target   = LLM_TOOL_ROUTE_VFS;
        out->priority = 0;
        strncpy(out->reason, "name_vfs", sizeof(out->reason) - 1);
    } else if (name_has_any(tool_name, hms_keys)) {
        out->target   = LLM_TOOL_ROUTE_HMS;
        out->priority = 0;
        strncpy(out->reason, "name_hms", sizeof(out->reason) - 1);
    } else if (name_has_any(tool_name, mb_keys)) {
        out->target   = LLM_TOOL_ROUTE_AGENT_MAILBOX;
        out->priority = 0;
        strncpy(out->reason, "name_mailbox", sizeof(out->reason) - 1);
    } else if (name_has_any(tool_name, skill_keys)) {
        out->target   = LLM_TOOL_ROUTE_SKILL_IFC;
        out->priority = 0;
        strncpy(out->reason, "name_skill", sizeof(out->reason) - 1);
    } else if (name_has_any(tool_name, net_keys)) {
        out->target   = LLM_TOOL_ROUTE_NET_HTTP;
        out->priority = 0;
        strncpy(out->reason, "name_net", sizeof(out->reason) - 1);
    } else {
        llm_tool_route_result_t hint = *out;
        args_hint(args_json, &hint);
        if (hint.target != LLM_TOOL_ROUTE_UNKNOWN) {
            *out = hint;
        } else {
            out->target   = LLM_TOOL_ROUTE_LOCAL_LLM;
            out->priority = 4;
            strncpy(out->reason, "fallback_local", sizeof(out->reason) - 1);
        }
    }

    out->reason[sizeof(out->reason) - 1] = '\0';
    return 0;
}

static const char *target_tag(llm_tool_route_target_t t) {
    switch (t) {
    case LLM_TOOL_ROUTE_HMS:            return "hms";
    case LLM_TOOL_ROUTE_AGENT_MAILBOX:  return "mailbox";
    case LLM_TOOL_ROUTE_VFS:            return "vfs";
    case LLM_TOOL_ROUTE_SKILL_IFC:      return "skill_ifc";
    case LLM_TOOL_ROUTE_NET_HTTP:       return "net_http";
    case LLM_TOOL_ROUTE_LOCAL_LLM:      return "local_llm";
    default:                            return "unknown";
    }
}

int llm_tool_route_format(const llm_tool_route_result_t *res,
                          char *buf, size_t buf_max) {
    if (!res || !buf || buf_max == 0)
        return -EINVAL;
    return snprintf(buf, buf_max, "tgt=%s pri=%u why=%s",
                    target_tag(res->target), (unsigned)res->priority, res->reason);
}
