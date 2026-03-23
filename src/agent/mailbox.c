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
