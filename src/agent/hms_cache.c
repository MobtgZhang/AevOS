#include "hms_cache.h"
#include "kernel/mm/slab.h"
#include "lib/string.h"

typedef struct l1_entry {
    char     key[64];
    uint8_t *blob;
    size_t   len;
    uint32_t ref;
    bool     in_use;
} l1_entry_t;

struct hms_l1 {
    l1_entry_t *tab;
    uint32_t    cap;
    uint32_t    hand; /* CLOCK hand */
};

void hms_cache_init(hms_cache_t *c, uint32_t l1_slots)
{
    if (!c)
        return;
    memset(c, 0, sizeof(*c));
    c->lock = SPINLOCK_INIT;
    c->l1_slots = l1_slots ? l1_slots : 32;
    struct hms_l1 *L = (struct hms_l1 *)kcalloc(1, sizeof(struct hms_l1));
    if (!L)
        return;
    L->cap = c->l1_slots;
    L->tab = (l1_entry_t *)kcalloc(L->cap, sizeof(l1_entry_t));
    if (!L->tab) {
        kfree(L);
        return;
    }
    c->l1_private = L;
}

void hms_cache_destroy(hms_cache_t *c)
{
    if (!c || !c->l1_private)
        return;
    struct hms_l1 *L = (struct hms_l1 *)c->l1_private;
    for (uint32_t i = 0; i < L->cap; i++)
        kfree(L->tab[i].blob);
    kfree(L->tab);
    kfree(L);
    c->l1_private = NULL;
}

static l1_entry_t *l1_find(struct hms_l1 *L, const char *key)
{
    for (uint32_t i = 0; i < L->cap; i++)
        if (L->tab[i].in_use && strcmp(L->tab[i].key, key) == 0)
            return &L->tab[i];
    return NULL;
}

static l1_entry_t *l1_evict(struct hms_l1 *L)
{
    for (uint32_t n = 0; n < L->cap * 2; n++) {
        l1_entry_t *e = &L->tab[L->hand];
        L->hand = (L->hand + 1) % L->cap;
        if (!e->in_use)
            continue;
        if (e->ref == 0) {
            kfree(e->blob);
            memset(e, 0, sizeof(*e));
            return e;
        }
        e->ref = 0;
    }
    /* fallback: slot 0 */
    l1_entry_t *e = &L->tab[0];
    kfree(e->blob);
    memset(e, 0, sizeof(*e));
    return e;
}

int hms_cache_l1_get(hms_cache_t *c, const char *key, void *out, size_t *io_len)
{
    if (!c || !c->l1_private || !key || !out || !io_len)
        return -EINVAL;
    struct hms_l1 *L = (struct hms_l1 *)c->l1_private;
    spin_lock(&c->lock);
    l1_entry_t *e = l1_find(L, key);
    if (!e) {
        c->misses++;
        spin_unlock(&c->lock);
        return -ENOENT;
    }
    size_t n = *io_len < e->len ? *io_len : e->len;
    memcpy(out, e->blob, n);
    *io_len = n;
    e->ref = 1;
    c->hits++;
    spin_unlock(&c->lock);
    return 0;
}

int hms_cache_l1_put(hms_cache_t *c, const char *key, const void *data, size_t len)
{
    if (!c || !c->l1_private || !key || !data || len == 0)
        return -EINVAL;
    struct hms_l1 *L = (struct hms_l1 *)c->l1_private;
    spin_lock(&c->lock);
    l1_entry_t *e = l1_find(L, key);
    if (!e) {
        e = l1_evict(L);
        strncpy(e->key, key, sizeof(e->key) - 1);
        e->key[sizeof(e->key) - 1] = '\0';
        e->in_use = true;
    }
    kfree(e->blob);
    e->blob = (uint8_t *)kmalloc(len);
    if (!e->blob) {
        e->in_use = false;
        spin_unlock(&c->lock);
        return -ENOMEM;
    }
    memcpy(e->blob, data, len);
    e->len = len;
    e->ref = 1;
    spin_unlock(&c->lock);
    return 0;
}
