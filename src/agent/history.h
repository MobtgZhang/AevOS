#pragma once

#include <aevos/types.h>
#include <aevos/config.h>

#define HIST_ROLE_USER      0
#define HIST_ROLE_ASSISTANT 1
#define HIST_ROLE_SYSTEM    2
#define HIST_ROLE_TOOL      3

typedef struct {
    uint64_t  timestamp;
    uint8_t   role;
    uint32_t  token_count;
    uint8_t  *data;
    uint32_t  data_len;
    uint32_t  raw_len;
} hist_entry_t;

typedef struct history {
    hist_entry_t ring[HIST_RING_SIZE];
    uint32_t     head;
    uint32_t     tail;
    uint32_t     window_tokens;
    uint32_t     max_tokens;
    spinlock_t   lock;
} history_t;

void     history_init(history_t *h, uint32_t max_tokens);
int      history_push(history_t *h, uint8_t role, const char *text);
ssize_t  history_get(history_t *h, uint32_t index, char *out_buf, size_t buf_size);
hist_entry_t *history_get_recent(history_t *h, uint32_t count, uint32_t *out_count);
uint32_t history_count(history_t *h);
void     history_clear(history_t *h);
uint32_t history_get_window_tokens(history_t *h);
int      history_search(history_t *h, const char *keyword,
                        uint32_t *results, uint32_t max_results);
ssize_t  history_serialize(history_t *h, void *buf, size_t size);
int      history_deserialize(history_t *h, const void *buf, size_t size);
