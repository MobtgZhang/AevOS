#include "hms_cache.h"
#include "kernel/mm/slab.h"
#include "lib/string.h"

typedef struct kv_entry {
    char      key[64];
    uint8_t  *blob;
    size_t    len;
    uint32_t  ref;
    uint64_t  lru_tag;
    bool      in_use;
} kv_entry_t;

typedef struct hms_kv {
    kv_entry_t *tab;
    uint32_t    cap;
    uint32_t    hand;
    uint64_t    clock;
} hms_kv_t;

static void kv_destroy(hms_kv_t *K)
{
    if (!K || !K->tab)
        return;
    for (uint32_t i = 0; i < K->cap; i++)
        kfree(K->tab[i].blob);
    kfree(K->tab);
    K->tab = NULL;
}

static kv_entry_t *kv_find(hms_kv_t *K, const char *key)
{
    for (uint32_t i = 0; i < K->cap; i++)
        if (K->tab[i].in_use && strcmp(K->tab[i].key, key) == 0)
            return &K->tab[i];
    return NULL;
}

/* CLOCK for L1 */
static kv_entry_t *l1_evict(hms_kv_t *L)
{
    for (uint32_t n = 0; n < L->cap * 2; n++) {
        kv_entry_t *e = &L->tab[L->hand];
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
    kv_entry_t *e = &L->tab[0];
    kfree(e->blob);
    memset(e, 0, sizeof(*e));
    return e;
}

/* LRU-ish for L2/L3: evict smallest lru_tag */
static kv_entry_t *lru_evict(hms_kv_t *K)
{
    uint32_t best_i = UINT32_MAX;
    uint64_t best_t = UINT64_MAX;
    for (uint32_t i = 0; i < K->cap; i++) {
        if (!K->tab[i].in_use)
            return &K->tab[i];
        if (K->tab[i].lru_tag < best_t) {
            best_t = K->tab[i].lru_tag;
            best_i = i;
        }
    }
    kv_entry_t *e = &K->tab[best_i];
    kfree(e->blob);
    memset(e, 0, sizeof(*e));
    return e;
}

void hms_cache_init(hms_cache_t *c, uint32_t l1_slots)
{
    if (!c)
        return;
    memset(c, 0, sizeof(*c));
    c->lock = SPINLOCK_INIT;
    c->l1_slots = l1_slots ? l1_slots : 32;
    c->l2_slots = 256;
    c->l3_slots = 512;

    hms_kv_t *L1 = (hms_kv_t *)kcalloc(1, sizeof(hms_kv_t));
    if (L1) {
        L1->cap = c->l1_slots;
        L1->tab = (kv_entry_t *)kcalloc(L1->cap, sizeof(kv_entry_t));
        if (!L1->tab) {
            kfree(L1);
        } else
            c->l1_private = L1;
    }

    hms_kv_t *L2 = (hms_kv_t *)kcalloc(1, sizeof(hms_kv_t));
    if (L2) {
        L2->cap = c->l2_slots;
        L2->tab = (kv_entry_t *)kcalloc(L2->cap, sizeof(kv_entry_t));
        if (!L2->tab) {
            kfree(L2);
        } else
            c->l2_private = L2;
    }

    hms_kv_t *L3 = (hms_kv_t *)kcalloc(1, sizeof(hms_kv_t));
    if (L3) {
        L3->cap = c->l3_slots;
        L3->tab = (kv_entry_t *)kcalloc(L3->cap, sizeof(kv_entry_t));
        if (!L3->tab) {
            kfree(L3);
        } else
            c->l3_private = L3;
    }
}

void hms_cache_destroy(hms_cache_t *c)
{
    if (!c)
        return;
    if (c->l1_private) {
        kv_destroy((hms_kv_t *)c->l1_private);
        kfree(c->l1_private);
        c->l1_private = NULL;
    }
    if (c->l2_private) {
        kv_destroy((hms_kv_t *)c->l2_private);
        kfree(c->l2_private);
        c->l2_private = NULL;
    }
    if (c->l3_private) {
        kv_destroy((hms_kv_t *)c->l3_private);
        kfree(c->l3_private);
        c->l3_private = NULL;
    }
}

int hms_cache_l1_get(hms_cache_t *c, const char *key, void *out, size_t *io_len)
{
    if (!c || !c->l1_private || !key || !out || !io_len)
        return -EINVAL;
    hms_kv_t *L = (hms_kv_t *)c->l1_private;
    spin_lock(&c->lock);
    kv_entry_t *e = kv_find(L, key);
    if (!e) {
        c->misses++;
        spin_unlock(&c->lock);
        return -ENOENT;
    }
    size_t n = *io_len < e->len ? *io_len : e->len;
    memcpy(out, e->blob, n);
    *io_len = n;
    e->ref = 1;
    c->hits_l1++;
    spin_unlock(&c->lock);
    return 0;
}

int hms_cache_l1_put(hms_cache_t *c, const char *key, const void *data, size_t len)
{
    if (!c || !c->l1_private || !key || !data || len == 0)
        return -EINVAL;
    hms_kv_t *L = (hms_kv_t *)c->l1_private;
    spin_lock(&c->lock);
    kv_entry_t *e = kv_find(L, key);
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

static int tier_get(hms_cache_t *c, void *priv, uint64_t *hits, const char *key,
                    void *out, size_t *io_len)
{
    if (!priv)
        return -EINVAL;
    hms_kv_t *L = (hms_kv_t *)priv;
    spin_lock(&c->lock);
    kv_entry_t *e = kv_find(L, key);
    if (!e) {
        c->misses++;
        spin_unlock(&c->lock);
        return -ENOENT;
    }
    size_t n = *io_len < e->len ? *io_len : e->len;
    memcpy(out, e->blob, n);
    *io_len = n;
    L->clock++;
    e->lru_tag = L->clock;
    (*hits)++;
    spin_unlock(&c->lock);
    return 0;
}

static int tier_put(hms_cache_t *c, void *priv, const char *key,
                    const void *data, size_t len)
{
    if (!priv)
        return -EINVAL;
    hms_kv_t *L = (hms_kv_t *)priv;
    spin_lock(&c->lock);
    kv_entry_t *e = kv_find(L, key);
    if (!e) {
        uint32_t used = 0;
        for (uint32_t i = 0; i < L->cap; i++)
            if (L->tab[i].in_use)
                used++;
        if (used >= L->cap)
            e = lru_evict(L);
        else {
            for (uint32_t i = 0; i < L->cap; i++) {
                if (!L->tab[i].in_use) {
                    e = &L->tab[i];
                    break;
                }
            }
        }
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
    L->clock++;
    e->lru_tag = L->clock;
    spin_unlock(&c->lock);
    return 0;
}

int hms_cache_l2_get(hms_cache_t *c, const char *key, void *out, size_t *io_len)
{
    if (!c || !key || !out || !io_len)
        return -EINVAL;
    return tier_get(c, c->l2_private, &c->hits_l2, key, out, io_len);
}

int hms_cache_l2_put(hms_cache_t *c, const char *key, const void *data, size_t len)
{
    if (!c || !key || !data || len == 0)
        return -EINVAL;
    return tier_put(c, c->l2_private, key, data, len);
}

int hms_cache_l3_get(hms_cache_t *c, const char *key, void *out, size_t *io_len)
{
    if (!c || !key || !out || !io_len)
        return -EINVAL;
    return tier_get(c, c->l3_private, &c->hits_l3, key, out, io_len);
}

int hms_cache_l3_put(hms_cache_t *c, const char *key, const void *data, size_t len)
{
    if (!c || !key || !data || len == 0)
        return -EINVAL;
    return tier_put(c, c->l3_private, key, data, len);
}
