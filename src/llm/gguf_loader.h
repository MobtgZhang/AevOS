#pragma once

#include <aevos/types.h>

struct llm_ctx;

typedef enum {
    GGUF_TYPE_F32    = 0,
    GGUF_TYPE_F16    = 1,
    GGUF_TYPE_Q4_0   = 2,
    GGUF_TYPE_Q4_1   = 3,
    GGUF_TYPE_Q5_0   = 6,
    GGUF_TYPE_Q5_1   = 7,
    GGUF_TYPE_Q8_0   = 8,
    GGUF_TYPE_Q8_1   = 9,
    GGUF_TYPE_Q2_K   = 10,
    GGUF_TYPE_Q3_K   = 11,
    GGUF_TYPE_Q4_K   = 12,
    GGUF_TYPE_Q5_K   = 13,
    GGUF_TYPE_Q6_K   = 14,
    GGUF_TYPE_Q4_K_M = 15,
} gguf_dtype_t;

typedef enum {
    GGUF_KV_UINT8   = 0,
    GGUF_KV_INT8    = 1,
    GGUF_KV_UINT16  = 2,
    GGUF_KV_INT16   = 3,
    GGUF_KV_UINT32  = 4,
    GGUF_KV_INT32   = 5,
    GGUF_KV_FLOAT32 = 6,
    GGUF_KV_BOOL    = 7,
    GGUF_KV_STRING  = 8,
    GGUF_KV_ARRAY   = 9,
    GGUF_KV_UINT64  = 10,
    GGUF_KV_INT64   = 11,
    GGUF_KV_FLOAT64 = 12,
} gguf_kv_type_t;

#define GGUF_MAGIC       0x46475547u
#define GGUF_MAX_DIMS    4
#define GGUF_MAX_KV      512
#define GGUF_MAX_TENSORS 1024
#define GGUF_MAX_KEY_LEN 256

typedef struct PACKED {
    uint32_t magic;
    uint32_t version;
    uint64_t n_tensors;
    uint64_t n_kv;
} gguf_header_t;

typedef struct {
    char            key[GGUF_MAX_KEY_LEN];
    gguf_kv_type_t  type;
    union {
        uint8_t   u8;
        int8_t    i8;
        uint16_t  u16;
        int16_t   i16;
        uint32_t  u32;
        int32_t   i32;
        uint64_t  u64;
        int64_t   i64;
        float     f32;
        double    f64;
        bool      b;
        struct { char *data; uint64_t len; } str;
        struct { gguf_kv_type_t elem_type; uint64_t count; void *data; } arr;
    } value;
} gguf_kv_t;

typedef struct {
    char         name[GGUF_MAX_KEY_LEN];
    uint32_t     n_dims;
    uint64_t     dims[GGUF_MAX_DIMS];
    gguf_dtype_t type;
    uint64_t     offset;
    size_t       size_bytes;
} gguf_tensor_info_t;

typedef struct {
    gguf_header_t       header;
    gguf_kv_t          *kv_pairs;
    uint32_t            n_kv;
    gguf_tensor_info_t *tensor_infos;
    uint32_t            n_tensors;
    uint64_t            data_offset;
    uint8_t            *file_data;
    size_t              file_size;
    /* If non-NULL, gguf_close() kfree()s this (whole file read from VFS). */
    uint8_t            *heap_data;
} gguf_file_t;

gguf_file_t        *gguf_open(const char *path);
gguf_file_t        *gguf_parse(const uint8_t *data, size_t size);
void                gguf_close(gguf_file_t *file);
gguf_kv_t          *gguf_get_kv(gguf_file_t *file, const char *key);
void               *gguf_get_tensor(gguf_file_t *file, const char *name);
gguf_tensor_info_t *gguf_find_tensor_info(gguf_file_t *file, const char *name);
int                 gguf_load_into_ctx(gguf_file_t *file, struct llm_ctx *ctx);
