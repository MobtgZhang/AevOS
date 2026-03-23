#include "hms_core.h"
#include "lib/string.h"

#define HMS_PACK_MAX 8

typedef struct {
    uint32_t     n;
    mem_result_t r[HMS_PACK_MAX];
} hms_mem_pack_t;

static void embed_query_key(const int8_t *emb, char *out, size_t out_sz)
{
    uint64_t h = 14695981039346656037ULL;
    int      lim = EMBED_DIM < 128 ? EMBED_DIM : 128;
    for (int i = 0; i < lim; i++)
        h = (h ^ (uint8_t)emb[i]) * 1099511628211ULL;
    snprintf(out, out_sz, "m:%016llx", (unsigned long long)h);
}

void hms_text_to_pseudo_embed(const char *text, int8_t *out, uint32_t dim)
{
    if (!out || dim == 0)
        return;
    memset(out, 0, dim);
    if (!text) {
        return;
    }
    uint32_t h = 5381u;
    for (const unsigned char *t = (const unsigned char *)text; *t; t++)
        h = h * 33u + *t;
    for (uint32_t i = 0; i < dim; i++) {
        h = h * 1664525u + 1013904223u;
        out[i] = (int8_t)((h >> 16) & 0xFF) - 64;
    }
}

static int pack_get(hms_cache_t *cache, const char *tierpfx, const char *sig,
                    hms_mem_pack_t *pack)
{
    char k[72];
    snprintf(k, sizeof(k), "%s:%s", tierpfx, sig);
    size_t n = sizeof(*pack);
    if (tierpfx[0] == '1')
        return hms_cache_l1_get(cache, k, pack, &n);
    if (tierpfx[0] == '2')
        return hms_cache_l2_get(cache, k, pack, &n);
    return hms_cache_l3_get(cache, k, pack, &n);
}

static void pack_put(hms_cache_t *cache, const char *tierpfx, const char *sig,
                     const hms_mem_pack_t *pack)
{
    char k[72];
    snprintf(k, sizeof(k), "%s:%s", tierpfx, sig);
    size_t n = sizeof(*pack);
    if (tierpfx[0] == '1')
        hms_cache_l1_put(cache, k, pack, n);
    else if (tierpfx[0] == '2')
        hms_cache_l2_put(cache, k, pack, n);
    else
        hms_cache_l3_put(cache, k, pack, n);
}

int hms_memory_retrieve_tiered(memory_engine_t *mem, hms_cache_t *cache,
                               const int8_t *qemb, uint32_t top_k,
                               mem_result_t *results, hms_mem_tier_t *resolved_tier)
{
    if (!mem || !cache || !qemb || !results)
        return -EINVAL;
    if (resolved_tier)
        *resolved_tier = HMS_MEM_TIER_NONE;

    if (top_k > HMS_PACK_MAX)
        top_k = HMS_PACK_MAX;

    char sig[32];
    embed_query_key(qemb, sig, sizeof(sig));

    hms_mem_pack_t pack;
    memset(&pack, 0, sizeof(pack));

    if (pack_get(cache, "1", sig, &pack) == 0 && pack.n > 0) {
        for (uint32_t i = 0; i < pack.n && i < top_k; i++)
            results[i] = pack.r[i];
        if (resolved_tier)
            *resolved_tier = HMS_MEM_TIER_L1;
        return (int)pack.n;
    }
    if (pack_get(cache, "2", sig, &pack) == 0 && pack.n > 0) {
        for (uint32_t i = 0; i < pack.n && i < top_k; i++)
            results[i] = pack.r[i];
        pack_put(cache, "1", sig, &pack);
        if (resolved_tier)
            *resolved_tier = HMS_MEM_TIER_L2;
        return (int)pack.n;
    }
    if (pack_get(cache, "3", sig, &pack) == 0 && pack.n > 0) {
        for (uint32_t i = 0; i < pack.n && i < top_k; i++)
            results[i] = pack.r[i];
        pack_put(cache, "2", sig, &pack);
        pack_put(cache, "1", sig, &pack);
        if (resolved_tier)
            *resolved_tier = HMS_MEM_TIER_L3;
        return (int)pack.n;
    }

    int found = memory_retrieve(mem, qemb, top_k, results);
    if (found > 0) {
        pack.n = (uint32_t)found;
        for (int i = 0; i < found; i++)
            pack.r[i] = results[i];
        pack_put(cache, "3", sig, &pack);
        pack_put(cache, "2", sig, &pack);
        pack_put(cache, "1", sig, &pack);
        if (resolved_tier)
            *resolved_tier = HMS_MEM_TIER_HNSW;
    }
    return found;
}

size_t hms_append_compressed_block(memory_engine_t *mem, history_t *hist,
                                     skill_engine_t *skills, hms_cache_t *cache,
                                     const char *user_text,
                                     char *out, size_t out_max)
{
    if (!out || out_max == 0)
        return 0;

    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, out_max - pos,
        "\n--- HMS compressed (Memory L1→L2→L3→HNSW; History B+ seq; Skill registry) ---\n");

    if (skills) {
        pos += (size_t)snprintf(out + pos, out_max - pos, "Skill registry: ");
        spin_lock(&skills->lock);
        for (uint32_t i = 0; i < skills->count && pos < out_max - 96; i++) {
            if (!skills->skills[i].active)
                continue;
            pos += (size_t)snprintf(out + pos, out_max - pos, "@%s#%llu ",
                skills->skills[i].name,
                (unsigned long long)skills->skills[i].id);
        }
        spin_unlock(&skills->lock);
        pos += (size_t)snprintf(out + pos, out_max - pos, "\n");
    }

    if (hist) {
        uint32_t hc = history_count(hist);
        pos += (size_t)snprintf(out + pos, out_max - pos,
            "History seq (B+): ");
        for (uint32_t k = 0; k < 6 && hc > k && pos < out_max - 48; k++) {
            uint32_t log_i = hc - 1 - k;
            uint64_t seq = history_seq_at(hist, log_i);
            pos += (size_t)snprintf(out + pos, out_max - pos, "[H:%llu] ",
                (unsigned long long)seq);
        }
        pos += (size_t)snprintf(out + pos, out_max - pos, "\n");
    }

    if (mem && cache && user_text) {
        int8_t emb[EMBED_DIM];
        hms_text_to_pseudo_embed(user_text, emb, EMBED_DIM);

        mem_result_t res[HMS_PACK_MAX];
        hms_mem_tier_t tier = HMS_MEM_TIER_NONE;
        int nf = hms_memory_retrieve_tiered(mem, cache, emb, HMS_PACK_MAX, res, &tier);

        static const char *tier_name[] = { "?", "L1", "L2", "L3", "HNSW" };
        const char *tn = (tier <= HMS_MEM_TIER_HNSW) ? tier_name[tier] : "?";

        pos += (size_t)snprintf(out + pos, out_max - pos,
            "Memory hits (tier=%s): ", tn);

        char snippet[96];
        for (int i = 0; i < nf && pos < out_max - 128; i++) {
            if (memory_fetch_by_id(mem, res[i].id, snippet, sizeof(snippet)) < 0)
                snippet[0] = '\0';
            /* collapse newlines for one-line token budget */
            for (char *p = snippet; *p; p++) {
                if (*p == '\n' || *p == '\r')
                    *p = ' ';
            }
            pos += (size_t)snprintf(out + pos, out_max - pos,
                "[M:%llu~%.2f] %.80s | ",
                (unsigned long long)res[i].id, res[i].score, snippet);
        }
        if (nf == 0)
            pos += (size_t)snprintf(out + pos, out_max - pos, "(empty) ");
        pos += (size_t)snprintf(out + pos, out_max - pos, "\n");
    }

    pos += (size_t)snprintf(out + pos, out_max - pos, "--- end HMS ---\n");
    if (pos >= out_max) {
        out[out_max - 1] = '\0';
        return out_max - 1;
    }
    return pos;
}
