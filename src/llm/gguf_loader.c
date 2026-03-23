#include "gguf_loader.h"
#include "llm_runtime.h"
#include "quantize.h"
#include "kernel/mm/slab.h"
#include "kernel/klog.h"
#include "lib/string.h"
#include "kernel/fs/vfs.h"

/* ── Per-element byte sizes for each quantization type ────────── */

static size_t gguf_type_block_size(gguf_dtype_t t) {
    switch (t) {
    case GGUF_TYPE_F32:    return 1;
    case GGUF_TYPE_F16:    return 1;
    case GGUF_TYPE_Q4_0:   return QK4_0;
    case GGUF_TYPE_Q8_0:   return QK8_0;
    case GGUF_TYPE_Q4_K:
    case GGUF_TYPE_Q4_K_M: return QK_K;
    default:               return 1;
    }
}

static size_t gguf_type_block_bytes(gguf_dtype_t t) {
    switch (t) {
    case GGUF_TYPE_F32:    return sizeof(float);
    case GGUF_TYPE_F16:    return sizeof(float16_t);
    case GGUF_TYPE_Q4_0:   return sizeof(q4_0_block_t);
    case GGUF_TYPE_Q8_0:   return sizeof(q8_0_block_t);
    case GGUF_TYPE_Q4_K:
    case GGUF_TYPE_Q4_K_M: return sizeof(q4_k_block_t);
    default:               return 4;
    }
}

/* ── Binary read helpers ─────────────────────────────────────── */

typedef struct {
    const uint8_t *data;
    size_t         size;
    size_t         pos;
} reader_t;

static bool reader_ok(reader_t *r, size_t need) {
    return r->pos + need <= r->size;
}

static uint8_t read_u8(reader_t *r) {
    if (!reader_ok(r, 1)) return 0;
    return r->data[r->pos++];
}

static uint32_t read_u32(reader_t *r) {
    if (!reader_ok(r, 4)) return 0;
    uint32_t v;
    memcpy(&v, r->data + r->pos, 4);
    r->pos += 4;
    return v;
}

static uint64_t read_u64(reader_t *r) {
    if (!reader_ok(r, 8)) return 0;
    uint64_t v;
    memcpy(&v, r->data + r->pos, 8);
    r->pos += 8;
    return v;
}

static float read_f32(reader_t *r) {
    if (!reader_ok(r, 4)) return 0.0f;
    float v;
    memcpy(&v, r->data + r->pos, 4);
    r->pos += 4;
    return v;
}

static void read_str(reader_t *r, char *out, size_t out_max) {
    uint64_t len = read_u64(r);
    if (len >= out_max) len = out_max - 1;
    if (!reader_ok(r, len)) { out[0] = 0; return; }
    memcpy(out, r->data + r->pos, len);
    out[len] = '\0';
    r->pos += len;
}

/* ── Read a single KV pair ───────────────────────────────────── */

static void read_kv(reader_t *r, gguf_kv_t *kv) {
    read_str(r, kv->key, GGUF_MAX_KEY_LEN);
    kv->type = (gguf_kv_type_t)read_u32(r);

    switch (kv->type) {
    case GGUF_KV_UINT8:   kv->value.u8  = read_u8(r);                     break;
    case GGUF_KV_INT8:    kv->value.i8  = (int8_t)read_u8(r);             break;
    case GGUF_KV_UINT16:  kv->value.u16 = (uint16_t)read_u32(r);          break;
    case GGUF_KV_INT16:   kv->value.i16 = (int16_t)read_u32(r);           break;
    case GGUF_KV_UINT32:  kv->value.u32 = read_u32(r);                    break;
    case GGUF_KV_INT32:   kv->value.i32 = (int32_t)read_u32(r);           break;
    case GGUF_KV_FLOAT32: kv->value.f32 = read_f32(r);                    break;
    case GGUF_KV_BOOL:    kv->value.b   = read_u8(r) != 0;                break;
    case GGUF_KV_UINT64:  kv->value.u64 = read_u64(r);                    break;
    case GGUF_KV_INT64:   kv->value.i64 = (int64_t)read_u64(r);           break;
    case GGUF_KV_FLOAT64: {
        uint64_t bits = read_u64(r);
        memcpy(&kv->value.f64, &bits, 8);
        break;
    }
    case GGUF_KV_STRING: {
        uint64_t slen = read_u64(r);
        kv->value.str.len = slen;
        kv->value.str.data = (char *)kmalloc(slen + 1);
        if (kv->value.str.data && reader_ok(r, slen)) {
            memcpy(kv->value.str.data, r->data + r->pos, slen);
            kv->value.str.data[slen] = '\0';
        }
        r->pos += slen;
        break;
    }
    case GGUF_KV_ARRAY: {
        kv->value.arr.elem_type = (gguf_kv_type_t)read_u32(r);
        kv->value.arr.count = read_u64(r);
        size_t elem_sz = 0;
        switch (kv->value.arr.elem_type) {
        case GGUF_KV_UINT8:  case GGUF_KV_INT8:  case GGUF_KV_BOOL: elem_sz = 1; break;
        case GGUF_KV_UINT16: case GGUF_KV_INT16: elem_sz = 2; break;
        case GGUF_KV_UINT32: case GGUF_KV_INT32: case GGUF_KV_FLOAT32: elem_sz = 4; break;
        case GGUF_KV_UINT64: case GGUF_KV_INT64: case GGUF_KV_FLOAT64: elem_sz = 8; break;
        default: elem_sz = 4; break;
        }
        size_t total = kv->value.arr.count * elem_sz;
        kv->value.arr.data = kmalloc(total);
        if (kv->value.arr.data && reader_ok(r, total))
            memcpy(kv->value.arr.data, r->data + r->pos, total);
        r->pos += total;
        break;
    }
    }
}

/* ── Read tensor info ────────────────────────────────────────── */

static void read_tensor_info(reader_t *r, gguf_tensor_info_t *ti) {
    read_str(r, ti->name, GGUF_MAX_KEY_LEN);
    ti->n_dims = read_u32(r);
    if (ti->n_dims > GGUF_MAX_DIMS) ti->n_dims = GGUF_MAX_DIMS;

    uint64_t n_elem = 1;
    for (uint32_t d = 0; d < ti->n_dims; d++) {
        ti->dims[d] = read_u64(r);
        n_elem *= ti->dims[d];
    }
    for (uint32_t d = ti->n_dims; d < GGUF_MAX_DIMS; d++)
        ti->dims[d] = 1;

    ti->type   = (gguf_dtype_t)read_u32(r);
    ti->offset = read_u64(r);

    size_t bs   = gguf_type_block_size(ti->type);
    size_t bb   = gguf_type_block_bytes(ti->type);
    ti->size_bytes = (n_elem / bs) * bb;
}

/* ── gguf_open ───────────────────────────────────────────────── */

gguf_file_t *gguf_open(const char *path) {
    if (!path || !path[0])
        return NULL;

    vfs_fd_t fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) {
        klog("gguf_open: vfs_open failed for '%s' (%d)\n", path, fd);
        return NULL;
    }

    vfs_stat_t st;
    if (vfs_fstat(fd, &st) < 0 || st.type != VFS_FILE || st.size == 0) {
        vfs_close(fd);
        klog("gguf_open: fstat failed or empty '%s'\n", path);
        return NULL;
    }

    uint8_t *buf = (uint8_t *)kmalloc((size_t)st.size);
    if (!buf) {
        vfs_close(fd);
        return NULL;
    }

    size_t got = 0;
    while (got < (size_t)st.size) {
        ssize_t n = vfs_read(fd, buf + got, (size_t)st.size - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    vfs_close(fd);

    if (got != (size_t)st.size) {
        klog("gguf_open: short read '%s' (%llu / %llu)\n", path,
             (unsigned long long)got, (unsigned long long)st.size);
        kfree(buf);
        return NULL;
    }

    gguf_file_t *gf = gguf_parse(buf, (size_t)st.size);
    if (!gf) {
        kfree(buf);
        return NULL;
    }

    gf->heap_data = buf;
    return gf;
}

/*
 * Core parsing logic, used once we have the raw data in memory.
 * Separated so it can be called from both gguf_open (once VFS is ready)
 * and from an external loader that provides a buffer.
 */
gguf_file_t *gguf_parse(const uint8_t *data, size_t size) {
    reader_t r = { data, size, 0 };

    uint32_t magic = read_u32(&r);
    if (magic != GGUF_MAGIC) {
        klog("gguf: bad magic 0x%08x (expected 0x%08x)\n", magic, GGUF_MAGIC);
        return NULL;
    }

    gguf_file_t *gf = (gguf_file_t *)kmalloc(sizeof(gguf_file_t));
    if (!gf) return NULL;
    memset(gf, 0, sizeof(*gf));

    gf->header.magic     = magic;
    gf->header.version   = read_u32(&r);
    gf->header.n_tensors = read_u64(&r);
    gf->header.n_kv      = read_u64(&r);
    gf->file_data        = (uint8_t *)data;
    gf->file_size        = size;

    if (gf->header.version < 2 || gf->header.version > 3) {
        klog("gguf: unsupported version %u\n", gf->header.version);
        kfree(gf);
        return NULL;
    }

    gf->n_kv = (uint32_t)MIN(gf->header.n_kv, GGUF_MAX_KV);
    gf->kv_pairs = (gguf_kv_t *)kcalloc(gf->n_kv, sizeof(gguf_kv_t));
    if (!gf->kv_pairs) { kfree(gf); return NULL; }

    for (uint32_t i = 0; i < gf->n_kv; i++)
        read_kv(&r, &gf->kv_pairs[i]);

    gf->n_tensors = (uint32_t)MIN(gf->header.n_tensors, GGUF_MAX_TENSORS);
    gf->tensor_infos = (gguf_tensor_info_t *)kcalloc(gf->n_tensors,
                                                      sizeof(gguf_tensor_info_t));
    if (!gf->tensor_infos) {
        kfree(gf->kv_pairs);
        kfree(gf);
        return NULL;
    }

    for (uint32_t i = 0; i < gf->n_tensors; i++)
        read_tensor_info(&r, &gf->tensor_infos[i]);

    /* Tensor data starts at the next 32-byte aligned offset */
    gf->data_offset = ALIGN_UP(r.pos, 32);

    klog("gguf: loaded v%u, %u kv pairs, %u tensors, data@0x%llx\n",
         gf->header.version, gf->n_kv, gf->n_tensors,
         (unsigned long long)gf->data_offset);
    return gf;
}

/* ── gguf_close ──────────────────────────────────────────────── */

void gguf_close(gguf_file_t *file) {
    if (!file) return;

    if (file->heap_data) {
        kfree(file->heap_data);
        file->heap_data = NULL;
        file->file_data = NULL;
        file->file_size = 0;
    }

    for (uint32_t i = 0; i < file->n_kv; i++) {
        if (file->kv_pairs[i].type == GGUF_KV_STRING)
            kfree(file->kv_pairs[i].value.str.data);
        else if (file->kv_pairs[i].type == GGUF_KV_ARRAY)
            kfree(file->kv_pairs[i].value.arr.data);
    }

    kfree(file->kv_pairs);
    kfree(file->tensor_infos);
    kfree(file);
}

/* ── Key-value lookup ────────────────────────────────────────── */

gguf_kv_t *gguf_get_kv(gguf_file_t *file, const char *key) {
    if (!file || !key) return NULL;
    for (uint32_t i = 0; i < file->n_kv; i++) {
        if (strcmp(file->kv_pairs[i].key, key) == 0)
            return &file->kv_pairs[i];
    }
    return NULL;
}

/* ── Tensor info lookup ──────────────────────────────────────── */

gguf_tensor_info_t *gguf_find_tensor_info(gguf_file_t *file, const char *name) {
    if (!file || !name) return NULL;
    for (uint32_t i = 0; i < file->n_tensors; i++) {
        if (strcmp(file->tensor_infos[i].name, name) == 0)
            return &file->tensor_infos[i];
    }
    return NULL;
}

/* ── Get raw tensor data pointer ─────────────────────────────── */

void *gguf_get_tensor(gguf_file_t *file, const char *name) {
    gguf_tensor_info_t *ti = gguf_find_tensor_info(file, name);
    if (!ti || !file->file_data) return NULL;
    return file->file_data + file->data_offset + ti->offset;
}

/* ── Helper: read uint32 KV with default ─────────────────────── */

static uint32_t kv_u32(gguf_file_t *gf, const char *key, uint32_t def) {
    gguf_kv_t *kv = gguf_get_kv(gf, key);
    if (!kv) return def;
    if (kv->type == GGUF_KV_UINT32) return kv->value.u32;
    if (kv->type == GGUF_KV_INT32)  return (uint32_t)kv->value.i32;
    return def;
}

static float kv_f32(gguf_file_t *gf, const char *key, float def) {
    gguf_kv_t *kv = gguf_get_kv(gf, key);
    return (kv && kv->type == GGUF_KV_FLOAT32) ? kv->value.f32 : def;
}

/* ── Load GGUF into LLM context ──────────────────────────────── */

int gguf_load_into_ctx(gguf_file_t *gf, struct llm_ctx *ctx) {
    if (!gf || !ctx) return -EINVAL;

    llm_config_t *cfg = &ctx->config;

    cfg->n_vocab   = kv_u32(gf, "llama.vocab_size",        32000);
    cfg->n_ctx     = kv_u32(gf, "llama.context_length",    LLM_DEFAULT_CTX);
    cfg->n_embd    = kv_u32(gf, "llama.embedding_length",  4096);
    cfg->n_layer   = kv_u32(gf, "llama.block_count",       32);
    cfg->n_head    = kv_u32(gf, "llama.attention.head_count", 32);
    cfg->n_head_kv = kv_u32(gf, "llama.attention.head_count_kv", cfg->n_head);
    cfg->n_ff      = kv_u32(gf, "llama.feed_forward_length", 11008);
    cfg->rope_theta = kv_f32(gf, "llama.rope.freq_base",    10000.0f);

    uint32_t kv_dim = (cfg->n_embd / cfg->n_head) * cfg->n_head_kv;
    size_t cache_size = (size_t)cfg->n_layer * cfg->n_ctx * kv_dim * sizeof(float);

    ctx->k_cache = (float *)kcalloc(1, cache_size);
    ctx->v_cache = (float *)kcalloc(1, cache_size);
    if (!ctx->k_cache || !ctx->v_cache) {
        klog("gguf: KV cache allocation failed (%llu bytes each)\n",
             (unsigned long long)cache_size);
        return -ENOMEM;
    }

    /* Map weight tensors — they point directly into the mmap'd file data */
    llm_weights_t *w = &ctx->weights;

    w->token_embd = gguf_get_tensor(gf, "token_embd.weight");
    w->rms_final  = (float *)gguf_get_tensor(gf, "output_norm.weight");
    w->output_weight = gguf_get_tensor(gf, "output.weight");

    w->wq      = (void **)kcalloc(cfg->n_layer, sizeof(void *));
    w->wk      = (void **)kcalloc(cfg->n_layer, sizeof(void *));
    w->wv      = (void **)kcalloc(cfg->n_layer, sizeof(void *));
    w->wo      = (void **)kcalloc(cfg->n_layer, sizeof(void *));
    w->w_gate  = (void **)kcalloc(cfg->n_layer, sizeof(void *));
    w->w_up    = (void **)kcalloc(cfg->n_layer, sizeof(void *));
    w->w_down  = (void **)kcalloc(cfg->n_layer, sizeof(void *));
    w->rms_att = (float **)kcalloc(cfg->n_layer, sizeof(float *));
    w->rms_ffn = (float **)kcalloc(cfg->n_layer, sizeof(float *));

    for (uint32_t l = 0; l < cfg->n_layer; l++) {
        char name[128];

        snprintf(name, sizeof(name), "blk.%u.attn_q.weight", l);
        w->wq[l] = gguf_get_tensor(gf, name);

        snprintf(name, sizeof(name), "blk.%u.attn_k.weight", l);
        w->wk[l] = gguf_get_tensor(gf, name);

        snprintf(name, sizeof(name), "blk.%u.attn_v.weight", l);
        w->wv[l] = gguf_get_tensor(gf, name);

        snprintf(name, sizeof(name), "blk.%u.attn_output.weight", l);
        w->wo[l] = gguf_get_tensor(gf, name);

        snprintf(name, sizeof(name), "blk.%u.ffn_gate.weight", l);
        w->w_gate[l] = gguf_get_tensor(gf, name);

        snprintf(name, sizeof(name), "blk.%u.ffn_up.weight", l);
        w->w_up[l] = gguf_get_tensor(gf, name);

        snprintf(name, sizeof(name), "blk.%u.ffn_down.weight", l);
        w->w_down[l] = gguf_get_tensor(gf, name);

        snprintf(name, sizeof(name), "blk.%u.attn_norm.weight", l);
        w->rms_att[l] = (float *)gguf_get_tensor(gf, name);

        snprintf(name, sizeof(name), "blk.%u.ffn_norm.weight", l);
        w->rms_ffn[l] = (float *)gguf_get_tensor(gf, name);
    }

    /* Determine quantization type from the first Q-weight tensor */
    gguf_tensor_info_t *t0 = gguf_find_tensor_info(gf, "blk.0.attn_q.weight");
    if (t0)
        w->quant_type = (uint8_t)t0->type;
    else
        w->quant_type = (uint8_t)GGUF_TYPE_F32;

    /* Allocate working buffers */
    size_t work_need = (size_t)cfg->n_embd * 5
                     + (size_t)cfg->n_head * cfg->n_ctx
                     + (size_t)cfg->n_ff * 2
                     + (size_t)cfg->n_vocab;
    ctx->work_buf_size = work_need * sizeof(float);
    ctx->work_buf = (float *)kmalloc(ctx->work_buf_size);

    ctx->logits = (float *)kmalloc(cfg->n_vocab * sizeof(float));
    ctx->embd   = (float *)kmalloc(cfg->n_embd  * sizeof(float));

    if (!ctx->work_buf || !ctx->logits || !ctx->embd)
        return -ENOMEM;

    /* Load vocabulary from GGUF tokenizer metadata */
    gguf_kv_t *tok_kv = gguf_get_kv(gf, "tokenizer.ggml.tokens");
    if (tok_kv && tok_kv->type == GGUF_KV_ARRAY) {
        ctx->vocab_size = (uint32_t)tok_kv->value.arr.count;
        ctx->vocab = (char **)kcalloc(ctx->vocab_size, sizeof(char *));
        /* Vocabulary strings are stored as a GGUF string array —
         * the actual parsing depends on the array element type.
         * For now we assign from the raw array data. */
    } else {
        ctx->vocab_size = cfg->n_vocab;
        ctx->vocab = (char **)kcalloc(ctx->vocab_size, sizeof(char *));
    }

    gguf_kv_t *score_kv = gguf_get_kv(gf, "tokenizer.ggml.scores");
    if (score_kv && score_kv->type == GGUF_KV_ARRAY) {
        ctx->vocab_scores = (float *)score_kv->value.arr.data;
    } else {
        ctx->vocab_scores = NULL;
    }

    ctx->pos = 0;
    ctx->has_new_token = false;

    klog("gguf: ctx loaded — vocab=%u embd=%u layers=%u heads=%u ctx=%u ff=%u\n",
         cfg->n_vocab, cfg->n_embd, cfg->n_layer, cfg->n_head,
         cfg->n_ctx, cfg->n_ff);
    return 0;
}
