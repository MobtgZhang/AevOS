#include "agent_layer.h"
#include "kernel/klog.h"
#include "lib/string.h"

const char *agent_tool_state_name(agent_tool_state_t s) {
    switch (s) {
    case AGENT_TOOL_READY:
        return "READY";
    case AGENT_TOOL_WAIT_IO:
        return "WAIT_IO";
    case AGENT_TOOL_WAIT_LLM:
        return "WAIT_LLM";
    case AGENT_TOOL_DETACH:
        return "DETACH";
    default:
        return "?";
    }
}

bool agent_tool_try_set_state(agent_t *agent, agent_tool_state_t expect,
                              agent_tool_state_t next) {
    if (!agent)
        return false;
    if (agent->tool_state != expect)
        return false;
    agent->tool_state = next;
    return true;
}

void agent_layer_log_mailbox(struct agent *agent, uint64_t from_id,
                             const char *summary) {
    if (!agent || !summary)
        return;
    char pay[192];
    snprintf(pay, sizeof(pay), "from=%llu %s", (unsigned long long)from_id,
             summary);
    eventlog_append(EVLOG_MAILBOX, agent->id, pay);
}
