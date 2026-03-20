#pragma once

#include <aevos/types.h>

struct llm_ctx;

/*
 * SafeTensors binary format:
 *   [8 bytes]  uint64 LE — JSON header length
 *   [N bytes]  UTF-8 JSON header (tensor metadata + __metadata__)
 *   [... ]     raw tensor data (contiguous, aligned)
 *
 * JSON header example:
 *   {
 *     "weight.token_embd": { "dtype":"F32", "shape":[32000,4096], "data_offsets":[0,524288000] },
 *     "__metadata__": { "format":"pt" }
 *   }
 */

typedef enum {
    ST_DTYPE_F32  = 0,
    ST_DTYPE_F16  = 1,
    ST_DTYPE_BF16 = 2,
    ST_DTYPE_I32  = 3,
    ST_DTYPE_I16  = 4,
    ST_DTYPE_I8   = 5,
    ST_DTYPE_U8   = 6,
    ST_DTYPE_BOOL = 7,
    ST_DTYPE_F64  = 8,
    ST_DTYPE_UNKNOWN = 255,
} st_dtype_t;

#define ST_MAX_TENSORS 1024
#define ST_MAX_NAME    256
#define ST_MAX_DIMS    4

typedef struct {
    char       name[ST_MAX_NAME];
    st_dtype_t dtype;
    uint32_t   n_dims;
    uint64_t   shape[ST_MAX_DIMS];
    uint64_t   data_offset;
    uint64_t   data_size;
} st_tensor_info_t;

typedef struct {
    st_tensor_info_t *tensors;
    uint32_t          n_tensors;
    uint8_t          *data_base;
    uint8_t          *file_data;
    size_t            file_size;
    uint64_t          header_size;
} st_file_t;

st_file_t        *st_open(const char *path);
st_file_t        *st_parse(const uint8_t *data, size_t size);
void              st_close(st_file_t *file);
st_tensor_info_t *st_find_tensor(st_file_t *file, const char *name);
void             *st_get_tensor_data(st_file_t *file, const char *name);
int               st_load_into_ctx(st_file_t *file, struct llm_ctx *ctx);
