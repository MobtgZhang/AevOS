#pragma once

#include <aevos/types.h>

typedef enum {
    EVLOG_USER_INPUT = 1,
    EVLOG_LLM_START,
    EVLOG_LLM_END,
    EVLOG_TOOL_CALL,
    EVLOG_TOOL_RESULT,
    EVLOG_CANCEL,
    EVLOG_ERROR,
} eventlog_type_t;

typedef struct {
    uint64_t          seq;
    uint64_t          ts_ms;
    eventlog_type_t   type;
    uint64_t          agent_id;
    char              payload[192];
} eventlog_record_t;

#define EVENTLOG_CAPACITY 4096

void eventlog_init(void);
void eventlog_append(eventlog_type_t type, uint64_t agent_id, const char *payload);
uint64_t eventlog_seq(void);
/* Copy up to max records ending at tail into out; returns count written. */
uint32_t eventlog_snapshot(eventlog_record_t *out, uint32_t max_records);
