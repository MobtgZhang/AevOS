#include "history.h"
#include "kernel/mm/slab.h"
#include "kernel/klog.h"
#include "kernel/drivers/timer.h"
#include "lib/string.h"
#include "lib/lz4.h"

/* Rough token estimate: ~4 chars per token for English text */
static uint32_t estimate_tokens(const char *text) {
    return ((uint32_t)strlen(text) + 3) / 4;
}

/* ── history_init ────────────────────────────────────────── */

void history_init(history_t *h, uint32_t max_tokens) {
    if (!h) return;
    memset(h, 0, sizeof(*h));
    h->max_tokens = max_tokens;
    h->lock = SPINLOCK_INIT;
    hist_bpt_init(&h->bpt);
    klog("history: ring + B+ index (seq→slot); WAL → history_wal / future\n");
}

/* Free compressed data owned by an entry */
static void entry_free(hist_entry_t *e) {
    if (e->data) {
        kfree(e->data);
        e->data = NULL;
    }
    e->data_len = 0;
    e->raw_len  = 0;
    e->token_count = 0;
}

/* Advance tail, discarding the oldest entry */
static void evict_oldest(history_t *h) {
    if (h->head == h->tail) return;
    hist_entry_t *e = &h->ring[h->tail % HIST_RING_SIZE];
    h->window_tokens -= e->token_count;
    entry_free(e);
    h->tail++;
}

/* ── history_push ────────────────────────────────────────── */

int history_push(history_t *h, uint8_t role, const char *text) {
    if (!h || !text) return -EINVAL;

    spin_lock(&h->lock);

    size_t raw_len = strlen(text);
    uint32_t tok_cnt = estimate_tokens(text);

    /* Allocate compressed buffer (worst case: slightly larger than input) */
    size_t comp_max = raw_len + raw_len / 255 + 16;
    uint8_t *comp = (uint8_t *)kmalloc(comp_max);
    if (!comp) { spin_unlock(&h->lock); return -ENOMEM; }

    ssize_t comp_len = lz4_compress(text, raw_len, comp, comp_max);
    if (comp_len <= 0) {
        /* Fallback: store uncompressed */
        memcpy(comp, text, raw_len);
        comp_len = (ssize_t)raw_len;
    }

    /* Shrink allocation to actual compressed size */
    uint8_t *final = (uint8_t *)krealloc(comp, (size_t)comp_len);
    if (final) comp = final;

    /* Evict old entries if ring is full or token window exceeded */
    bool bpt_dirty = false;
    while (h->head - h->tail >= HIST_RING_SIZE) {
        evict_oldest(h);
        bpt_dirty = true;
    }
    while (h->window_tokens + tok_cnt > h->max_tokens && h->head > h->tail) {
        evict_oldest(h);
        bpt_dirty = true;
    }
    if (bpt_dirty)
        hist_bpt_rebuild(&h->bpt, h);

    /* Insert new entry */
    uint32_t idx = h->head % HIST_RING_SIZE;
    hist_entry_t *e = &h->ring[idx];

    /* Free any residual data at this slot (should be empty after eviction) */
    entry_free(e);

    e->timestamp   = timer_get_ms();
    e->seq         = h->next_seq++;
    e->role        = role;
    e->token_count = tok_cnt;
    e->data        = comp;
    e->data_len    = (uint32_t)comp_len;
    e->raw_len     = (uint32_t)raw_len;

    h->head++;
    h->window_tokens += tok_cnt;

    hist_bpt_insert(&h->bpt, e->seq, idx);

    spin_unlock(&h->lock);
    return 0;
}

/* ── history_get — decompress entry at logical index ─────── */

ssize_t history_get(history_t *h, uint32_t index, char *out_buf, size_t buf_size) {
    if (!h || !out_buf || buf_size == 0) return -EINVAL;

    spin_lock(&h->lock);

    if (index >= h->head - h->tail) {
        spin_unlock(&h->lock);
        return -ENOENT;
    }

    uint32_t abs_idx = (h->tail + index) % HIST_RING_SIZE;
    hist_entry_t *e = &h->ring[abs_idx];

    if (!e->data || e->data_len == 0) {
        spin_unlock(&h->lock);
        return -ENOENT;
    }

    ssize_t decomp_len = lz4_decompress(e->data, e->data_len, out_buf,
                                        MIN(buf_size - 1, (size_t)e->raw_len));
    if (decomp_len <= 0) {
        /* Fallback: data might be stored uncompressed */
        size_t copy_len = MIN(e->data_len, (uint32_t)(buf_size - 1));
        memcpy(out_buf, e->data, copy_len);
        out_buf[copy_len] = '\0';
        spin_unlock(&h->lock);
        return (ssize_t)copy_len;
    }

    out_buf[decomp_len] = '\0';
    spin_unlock(&h->lock);
    return decomp_len;
}

/* ── history_get_recent ──────────────────────────────────── */

hist_entry_t *history_get_recent(history_t *h, uint32_t count, uint32_t *out_count) {
    if (!h) { if (out_count) *out_count = 0; return NULL; }

    spin_lock(&h->lock);
    uint32_t total = h->head - h->tail;
    uint32_t n = (count > total) ? total : count;

    hist_entry_t *result = (hist_entry_t *)kmalloc(n * sizeof(hist_entry_t));
    if (!result) {
        spin_unlock(&h->lock);
        if (out_count) *out_count = 0;
        return NULL;
    }

    for (uint32_t i = 0; i < n; i++) {
        uint32_t abs_idx = (h->head - n + i) % HIST_RING_SIZE;
        result[i] = h->ring[abs_idx];
        /* Don't copy data pointer ownership — caller must not free */
    }

    spin_unlock(&h->lock);
    if (out_count) *out_count = n;
    return result;
}

/* ── history_count ───────────────────────────────────────── */

uint32_t history_count(history_t *h) {
    if (!h) return 0;
    return h->head - h->tail;
}

int history_truncate_keep(history_t *h, uint32_t keep_count) {
    if (!h) return -EINVAL;

    spin_lock(&h->lock);
    uint32_t total = h->head - h->tail;
    if (keep_count >= total) {
        spin_unlock(&h->lock);
        return 0;
    }
    uint32_t drop = total - keep_count;
    for (uint32_t i = 0; i < drop; i++) {
        if (h->head <= h->tail)
            break;
        h->head--;
        uint32_t idx = h->head % HIST_RING_SIZE;
        hist_entry_t *e = &h->ring[idx];
        h->window_tokens -= e->token_count;
        entry_free(e);
        memset(e, 0, sizeof(*e));
    }
    hist_bpt_rebuild(&h->bpt, h);
    spin_unlock(&h->lock);
    return 0;
}

/* ── history_clear ───────────────────────────────────────── */

void history_clear(history_t *h) {
    if (!h) return;
    spin_lock(&h->lock);
    while (h->head > h->tail) evict_oldest(h);
    h->head = 0;
    h->tail = 0;
    h->window_tokens = 0;
    h->next_seq = 0;
    hist_bpt_clear(&h->bpt);
    spin_unlock(&h->lock);
}

/* ── history_get_window_tokens ───────────────────────────── */

uint32_t history_get_window_tokens(history_t *h) {
    return h ? h->window_tokens : 0;
}

uint64_t history_seq_at(history_t *h, uint32_t logical_index) {
    if (!h)
        return 0;
    spin_lock(&h->lock);
    uint32_t total = h->head - h->tail;
    if (logical_index >= total) {
        spin_unlock(&h->lock);
        return 0;
    }
    uint32_t abs_idx = (h->tail + logical_index) % HIST_RING_SIZE;
    uint64_t s = h->ring[abs_idx].seq;
    spin_unlock(&h->lock);
    return s;
}

/* ── history_search — keyword search across entries ──────── */

int history_search(history_t *h, const char *keyword,
                   uint32_t *results, uint32_t max_results) {
    if (!h || !keyword || !results) return -EINVAL;

    spin_lock(&h->lock);
    uint32_t total = h->head - h->tail;
    uint32_t found = 0;

    /* Temporary decompression buffer */
    size_t buf_size = 8192;
    char *buf = (char *)kmalloc(buf_size);
    if (!buf) { spin_unlock(&h->lock); return -ENOMEM; }

    for (uint32_t i = 0; i < total && found < max_results; i++) {
        uint32_t abs_idx = (h->tail + i) % HIST_RING_SIZE;
        hist_entry_t *e = &h->ring[abs_idx];
        if (!e->data) continue;

        /* Ensure buffer is large enough */
        if (e->raw_len + 1 > buf_size) {
            kfree(buf);
            buf_size = e->raw_len + 1;
            buf = (char *)kmalloc(buf_size);
            if (!buf) break;
        }

        ssize_t len = lz4_decompress(e->data, e->data_len, buf,
                                     MIN(buf_size - 1, (size_t)e->raw_len));
        if (len <= 0) {
            size_t cl = MIN((size_t)e->data_len, buf_size - 1);
            memcpy(buf, e->data, cl);
            len = (ssize_t)cl;
        }
        buf[len] = '\0';

        if (strstr(buf, keyword))
            results[found++] = i;
    }

    kfree(buf);
    spin_unlock(&h->lock);
    return (int)found;
}

/* ── Serialization format ────────────────────────────────── */
/* Header: [magic 4B][count 4B][max_tokens 4B][window_tokens 4B]
 * Per entry: [timestamp 8B][role 1B][token_count 4B][raw_len 4B]
 *            [data_len 4B][data data_len B]
 */

#define HIST_SERIAL_MAGIC    0x48495354u /* "HIST" v1 */
#define HIST_SERIAL_MAGIC_V2 0x32534948u /* "HIS2" v2 + seq + next_seq */

ssize_t history_serialize(history_t *h, void *buf, size_t size) {
    if (!h || !buf) return -EINVAL;

    spin_lock(&h->lock);
    uint32_t count = h->head - h->tail;

    /* Calculate needed size (v2: +8 next_seq, +8 seq per entry) */
    size_t needed = 24; /* magic + count + max + win + next_seq */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (h->tail + i) % HIST_RING_SIZE;
        needed += 29 + h->ring[idx].data_len;
    }

    if (size < needed) { spin_unlock(&h->lock); return (ssize_t)needed; }

    uint8_t *p = (uint8_t *)buf;
    uint32_t magic = HIST_SERIAL_MAGIC_V2;
    memcpy(p, &magic, 4);           p += 4;
    memcpy(p, &count, 4);           p += 4;
    memcpy(p, &h->max_tokens, 4);   p += 4;
    memcpy(p, &h->window_tokens, 4); p += 4;
    memcpy(p, &h->next_seq, 8);     p += 8;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (h->tail + i) % HIST_RING_SIZE;
        hist_entry_t *e = &h->ring[idx];

        memcpy(p, &e->timestamp, 8);   p += 8;
        memcpy(p, &e->seq, 8);         p += 8;
        *p++ = e->role;
        memcpy(p, &e->token_count, 4); p += 4;
        memcpy(p, &e->raw_len, 4);     p += 4;
        memcpy(p, &e->data_len, 4);    p += 4;
        if (e->data && e->data_len > 0) {
            memcpy(p, e->data, e->data_len);
            p += e->data_len;
        }
    }

    spin_unlock(&h->lock);
    return (ssize_t)(p - (uint8_t *)buf);
}

int history_deserialize(history_t *h, const void *buf, size_t size) {
    if (!h || !buf || size < 16) return -EINVAL;

    const uint8_t *p = (const uint8_t *)buf;
    uint32_t magic;
    memcpy(&magic, p, 4); p += 4;
    bool v2 = (magic == HIST_SERIAL_MAGIC_V2);
    if (magic != HIST_SERIAL_MAGIC && !v2) return -EINVAL;

    uint32_t count, max_tok, win_tok;
    memcpy(&count, p, 4);   p += 4;
    memcpy(&max_tok, p, 4); p += 4;
    memcpy(&win_tok, p, 4); p += 4;

    history_clear(h);
    h->max_tokens = max_tok;
    (void)win_tok;

    if (v2) {
        if ((size_t)(p - (const uint8_t *)buf) + 8 > size)
            return -EINVAL;
        memcpy(&h->next_seq, p, 8);
        p += 8;
    }

    for (uint32_t i = 0; i < count; i++) {
        size_t hdr = v2 ? 29 : 21;
        if ((size_t)(p - (const uint8_t *)buf) + hdr > size) break;

        uint64_t ts, seq = 0;
        uint8_t role;
        uint32_t tok_cnt, raw_len, data_len;

        memcpy(&ts, p, 8);        p += 8;
        if (v2) {
            memcpy(&seq, p, 8);   p += 8;
        }
        role = *p++;
        memcpy(&tok_cnt, p, 4);   p += 4;
        memcpy(&raw_len, p, 4);   p += 4;
        memcpy(&data_len, p, 4);  p += 4;

        if ((size_t)(p - (const uint8_t *)buf) + data_len > size) break;

        uint32_t idx = h->head % HIST_RING_SIZE;
        hist_entry_t *e = &h->ring[idx];
        entry_free(e);

        e->timestamp   = ts;
        e->seq         = v2 ? seq : h->next_seq++;
        e->role        = role;
        e->token_count = tok_cnt;
        e->raw_len     = raw_len;
        e->data_len    = data_len;
        e->data = (uint8_t *)kmalloc(data_len);
        if (e->data && data_len > 0)
            memcpy(e->data, p, data_len);
        p += data_len;

        h->head++;
        h->window_tokens += tok_cnt;
        hist_bpt_insert(&h->bpt, e->seq, idx);
    }

    return 0;
}
