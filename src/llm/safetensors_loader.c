#include "safetensors_loader.h"
#include "llm_runtime.h"
#include "quantize.h"
#include "kernel/mm/slab.h"
#include "kernel/klog.h"
#include "lib/string.h"
#include "lib/json.h"

/* ── Dtype string → enum ────────────────────────────────── */

static st_dtype_t parse_dtype(const char *s) {
    if (!s) return ST_DTYPE_UNKNOWN;
    if (strcmp(s, "F32")  == 0) return ST_DTYPE_F32;
    if (strcmp(s, "F16")  == 0) return ST_DTYPE_F16;
    if (strcmp(s, "BF16") == 0) return ST_DTYPE_BF16;
    if (strcmp(s, "I32")  == 0) return ST_DTYPE_I32;
    if (strcmp(s, "I16")  == 0) return ST_DTYPE_I16;
    if (strcmp(s, "I8")   == 0) return ST_DTYPE_I8;
    if (strcmp(s, "U8")   == 0) return ST_DTYPE_U8;
    if (strcmp(s, "BOOL") == 0) return ST_DTYPE_BOOL;
    if (strcmp(s, "F64")  == 0) return ST_DTYPE_F64;
    return ST_DTYPE_UNKNOWN;
}

/* ── st_open (VFS placeholder) ──────────────────────────── */

st_file_t *st_open(const char *path) {
    (void)path;
    klog("st_open: VFS not yet available for '%s'\n", path);
    return NULL;
}

/* ── st_parse: parse from in-memory buffer ──────────────── */

st_file_t *st_parse(const uint8_t *data, size_t size) {
    if (!data || size < 8) {
        klog("safetensors: buffer too small\n");
        return NULL;
    }

    uint64_t hdr_len;
    memcpy(&hdr_len, data, 8);

    if (hdr_len == 0 || 8 + hdr_len > size) {
        klog("safetensors: invalid header length %llu\n",
             (unsigned long long)hdr_len);
        return NULL;
    }

    /* Parse JSON header */
    char *json_str = (char *)kmalloc(hdr_len + 1);
    if (!json_str) return NULL;
    memcpy(json_str, data + 8, hdr_len);
    json_str[hdr_len] = '\0';

    json_value_t *root = json_parse(json_str);
    kfree(json_str);

    if (!root || root->type != JSON_OBJECT) {
        klog("safetensors: failed to parse JSON header\n");
        if (root) json_free(root);
        return NULL;
    }

    st_file_t *sf = (st_file_t *)kmalloc(sizeof(st_file_t));
    if (!sf) { json_free(root); return NULL; }
    memset(sf, 0, sizeof(*sf));

    sf->file_data   = (uint8_t *)data;
    sf->file_size   = size;
    sf->header_size = hdr_len;
    sf->data_base   = (uint8_t *)(data + 8 + hdr_len);
    sf->tensors     = (st_tensor_info_t *)kcalloc(ST_MAX_TENSORS,
                                                    sizeof(st_tensor_info_t));
    if (!sf->tensors) {
        json_free(root);
        kfree(sf);
        return NULL;
    }

    /* Walk JSON object keys to find tensor entries */
    sf->n_tensors = 0;

    for (uint32_t i = 0; i < (uint32_t)root->object.count && sf->n_tensors < ST_MAX_TENSORS; i++) {
        const char *key = root->object.keys[i];
        json_value_t *val = root->object.values[i];

        /* Skip __metadata__ */
        if (key[0] == '_' && key[1] == '_') continue;
        if (val->type != JSON_OBJECT) continue;

        st_tensor_info_t *ti = &sf->tensors[sf->n_tensors];
        strncpy(ti->name, key, ST_MAX_NAME - 1);
        ti->name[ST_MAX_NAME - 1] = '\0';

        /* dtype */
        json_value_t *dtype_val = json_get(val, "dtype");
        if (dtype_val && dtype_val->type == JSON_STRING)
            ti->dtype = parse_dtype(dtype_val->str_val);
        else
            ti->dtype = ST_DTYPE_UNKNOWN;

        /* shape */
        json_value_t *shape_val = json_get(val, "shape");
        ti->n_dims = 0;
        if (shape_val && shape_val->type == JSON_ARRAY) {
            for (uint32_t d = 0; d < (uint32_t)shape_val->array.count && d < ST_MAX_DIMS; d++) {
                if (shape_val->array.items[d]->type == JSON_NUMBER) {
                    ti->shape[d] = (uint64_t)shape_val->array.items[d]->num_val;
                    ti->n_dims = d + 1;
                }
            }
        }

        /* data_offsets: [start, end] */
        json_value_t *offsets = json_get(val, "data_offsets");
        if (offsets && offsets->type == JSON_ARRAY && offsets->array.count >= 2) {
            uint64_t start = (uint64_t)offsets->array.items[0]->num_val;
            uint64_t end   = (uint64_t)offsets->array.items[1]->num_val;
            ti->data_offset = start;
            ti->data_size   = end - start;
        }

        sf->n_tensors++;
    }

    json_free(root);

    klog("safetensors: parsed %u tensors, data starts at offset %llu\n",
         sf->n_tensors, (unsigned long long)(8 + hdr_len));
    return sf;
}

/* ── st_close ────────────────────────────────────────────── */

void st_close(st_file_t *file) {
    if (!file) return;
    kfree(file->tensors);
    kfree(file);
}

/* ── Tensor lookup ───────────────────────────────────────── */

st_tensor_info_t *st_find_tensor(st_file_t *file, const char *name) {
    if (!file || !name) return NULL;
    for (uint32_t i = 0; i < file->n_tensors; i++) {
        if (strcmp(file->tensors[i].name, name) == 0)
            return &file->tensors[i];
    }
    return NULL;
}

void *st_get_tensor_data(st_file_t *file, const char *name) {
    st_tensor_info_t *ti = st_find_tensor(file, name);
    if (!ti || !file->data_base) return NULL;
    return file->data_base + ti->data_offset;
}

/* ── Load SafeTensors into LLM context ───────────────────── */

int st_load_into_ctx(st_file_t *sf, struct llm_ctx *ctx) {
    if (!sf || !ctx) return -EINVAL;

    llm_config_t *cfg = &ctx->config;

    /*
     * SafeTensors from HuggingFace models use naming like:
     *   model.embed_tokens.weight, model.layers.0.self_attn.q_proj.weight, etc.
     *
     * We detect the architecture from available tensors and map accordingly.
     */

    /* Try to infer dimensions from the embedding tensor */
    st_tensor_info_t *embd_ti = st_find_tensor(sf, "model.embed_tokens.weight");
    if (!embd_ti) embd_ti = st_find_tensor(sf, "token_embd.weight");

    if (embd_ti && embd_ti->n_dims >= 2) {
        cfg->n_vocab = (uint32_t)embd_ti->shape[0];
        cfg->n_embd  = (uint32_t)embd_ti->shape[1];
    } else {
        cfg->n_vocab = 32000;
        cfg->n_embd  = 4096;
    }

    /* Count layers */
    uint32_t max_layer = 0;
    for (uint32_t i = 0; i < sf->n_tensors; i++) {
        const char *name = sf->tensors[i].name;
        const char *p = NULL;
        /* Look for "layers.N." or "blk.N." */
        for (const char *s = name; *s; s++) {
            if ((s[0] == 'l' && s[1] == 'a' && s[2] == 'y' && s[3] == 'e' &&
                 s[4] == 'r' && s[5] == 's' && s[6] == '.') ||
                (s[0] == 'b' && s[1] == 'l' && s[2] == 'k' && s[3] == '.')) {
                p = (s[0] == 'b') ? s + 4 : s + 7;
                break;
            }
        }
        if (p) {
            uint32_t n = 0;
            while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
            if (n + 1 > max_layer) max_layer = n + 1;
        }
    }
    if (max_layer > 0) cfg->n_layer = max_layer;
    else cfg->n_layer = 32;

    cfg->n_head    = 32;
    cfg->n_head_kv = cfg->n_head;
    cfg->n_ff      = cfg->n_embd * 11008 / 4096;
    cfg->n_ctx     = LLM_DEFAULT_CTX;
    cfg->rope_theta = 10000.0f;

    /* Allocate KV cache */
    uint32_t kv_dim = (cfg->n_embd / cfg->n_head) * cfg->n_head_kv;
    size_t cache_size = (size_t)cfg->n_layer * cfg->n_ctx * kv_dim * sizeof(float);

    ctx->k_cache = (float *)kcalloc(1, cache_size);
    ctx->v_cache = (float *)kcalloc(1, cache_size);

    /* Map weight tensors — point directly into the mmap'd file data */
    llm_weights_t *w = &ctx->weights;
    w->quant_type = 0; /* F32 for safetensors */

    w->token_embd = st_get_tensor_data(sf, "model.embed_tokens.weight");
    if (!w->token_embd) w->token_embd = st_get_tensor_data(sf, "token_embd.weight");

    w->rms_final = (float *)st_get_tensor_data(sf, "model.norm.weight");
    if (!w->rms_final) w->rms_final = (float *)st_get_tensor_data(sf, "output_norm.weight");

    w->output_weight = st_get_tensor_data(sf, "lm_head.weight");
    if (!w->output_weight) w->output_weight = st_get_tensor_data(sf, "output.weight");

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

        /* HuggingFace naming convention */
        snprintf(name, sizeof(name), "model.layers.%u.self_attn.q_proj.weight", l);
        w->wq[l] = st_get_tensor_data(sf, name);

        snprintf(name, sizeof(name), "model.layers.%u.self_attn.k_proj.weight", l);
        w->wk[l] = st_get_tensor_data(sf, name);

        snprintf(name, sizeof(name), "model.layers.%u.self_attn.v_proj.weight", l);
        w->wv[l] = st_get_tensor_data(sf, name);

        snprintf(name, sizeof(name), "model.layers.%u.self_attn.o_proj.weight", l);
        w->wo[l] = st_get_tensor_data(sf, name);

        snprintf(name, sizeof(name), "model.layers.%u.mlp.gate_proj.weight", l);
        w->w_gate[l] = st_get_tensor_data(sf, name);

        snprintf(name, sizeof(name), "model.layers.%u.mlp.up_proj.weight", l);
        w->w_up[l] = st_get_tensor_data(sf, name);

        snprintf(name, sizeof(name), "model.layers.%u.mlp.down_proj.weight", l);
        w->w_down[l] = st_get_tensor_data(sf, name);

        snprintf(name, sizeof(name), "model.layers.%u.input_layernorm.weight", l);
        w->rms_att[l] = (float *)st_get_tensor_data(sf, name);

        snprintf(name, sizeof(name), "model.layers.%u.post_attention_layernorm.weight", l);
        w->rms_ffn[l] = (float *)st_get_tensor_data(sf, name);
    }

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

    ctx->pos = 0;
    ctx->has_new_token = false;

    klog("safetensors: ctx loaded — vocab=%u embd=%u layers=%u heads=%u\n",
         cfg->n_vocab, cfg->n_embd, cfg->n_layer, cfg->n_head);
    return 0;
}
