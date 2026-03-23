#pragma once

/*
 * L3 Agent Layer — 统一入口（Runtime + Self-Evolution Plane 子模块见各头文件）。
 * EventLog / Mailbox / 四态工具 / Cancel 与 sep_plane / evolution 协同。
 */
#include "agent_core.h"
#include "eventlog.h"
#include "mailbox.h"
#include "sep_plane.h"

const char *agent_tool_state_name(agent_tool_state_t s);
bool        agent_tool_try_set_state(agent_t *agent, agent_tool_state_t expect,
                                     agent_tool_state_t next);

void agent_layer_log_mailbox(struct agent *agent, uint64_t from_id,
                             const char *summary);
