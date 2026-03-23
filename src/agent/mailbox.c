#include "mailbox.h"
#include "lib/string.h"

void mailbox_init(mailbox_t *mb)
{
    if (!mb)
        return;
    memset(mb, 0, sizeof(*mb));
    mb->lock = SPINLOCK_INIT;
}

int mailbox_send(mailbox_t *mb, uint64_t from, uint64_t to,
                 const void *data, size_t len)
{
    if (!mb || !data || len == 0)
        return -EINVAL;
    if (len > MAILBOX_MSG_MAX - 1)
        len = MAILBOX_MSG_MAX - 1;

    spin_lock(&mb->lock);
    uint32_t slot = (mb->head + mb->count) % MAILBOX_CAP;
    if (mb->count < MAILBOX_CAP)
        mb->count++;
    else
        mb->head = (mb->head + 1) % MAILBOX_CAP;

    mailbox_msg_t *m = &mb->ring[slot];
    m->from_agent = from;
    m->to_agent   = to;
    memcpy(m->body, data, len);
    m->body[len] = '\0';
    m->len = (uint32_t)len;
    spin_unlock(&mb->lock);
    return 0;
}

int mailbox_broadcast(mailbox_t *mb, uint64_t from,
                      const void *data, size_t len)
{
    return mailbox_send(mb, from, MAILBOX_TO_ANY, data, len);
}

int mailbox_try_recv(mailbox_t *mb, mailbox_msg_t *out)
{
    if (!mb || !out)
        return -EINVAL;
    spin_lock(&mb->lock);
    if (mb->count == 0) {
        spin_unlock(&mb->lock);
        return -ENOENT;
    }
    *out = mb->ring[mb->head];
    mb->head = (mb->head + 1) % MAILBOX_CAP;
    mb->count--;
    spin_unlock(&mb->lock);
    return 0;
}

int mailbox_try_recv_for(mailbox_t *mb, uint64_t for_agent,
                         mailbox_msg_t *out)
{
    if (!mb || !out)
        return -EINVAL;
    spin_lock(&mb->lock);
    uint32_t n = mb->count;
    if (n == 0) {
        spin_unlock(&mb->lock);
        return -ENOENT;
    }

    uint32_t found_off = UINT32_MAX;
    for (uint32_t i = 0; i < n; i++) {
        mailbox_msg_t *m = &mb->ring[(mb->head + i) % MAILBOX_CAP];
        if (m->to_agent == MAILBOX_TO_ANY || m->to_agent == for_agent) {
            found_off = i;
            break;
        }
    }

    if (found_off == UINT32_MAX) {
        spin_unlock(&mb->lock);
        return -ENOENT;
    }

    *out = mb->ring[(mb->head + found_off) % MAILBOX_CAP];
    for (uint32_t k = found_off; k + 1 < n; k++) {
        uint32_t dst = (mb->head + k) % MAILBOX_CAP;
        uint32_t src = (mb->head + k + 1) % MAILBOX_CAP;
        mb->ring[dst] = mb->ring[src];
    }
    mb->count--;
    spin_unlock(&mb->lock);
    return 0;
}
