#pragma once

#include <aevos/types.h>

#define MAILBOX_CAP 256
#define MAILBOX_MSG_MAX 512

typedef struct {
    char     body[MAILBOX_MSG_MAX];
    uint32_t len;
    uint64_t from_agent;
    uint64_t to_agent;
} mailbox_msg_t;

typedef struct {
    mailbox_msg_t ring[MAILBOX_CAP];
    uint32_t      head;
    uint32_t      count;
    spinlock_t    lock;
} mailbox_t;

void mailbox_init(mailbox_t *mb);
int  mailbox_send(mailbox_t *mb, uint64_t from, uint64_t to,
                  const void *data, size_t len);
int  mailbox_try_recv(mailbox_t *mb, mailbox_msg_t *out);
