#pragma once

#include <aevos/types.h>

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type_t;

typedef struct json_value {
    json_type_t type;
    union {
        bool   bool_val;
        double num_val;
        char  *str_val;
        struct {
            struct json_value **items;
            size_t              count;
        } array;
        struct {
            char              **keys;
            struct json_value **values;
            size_t              count;
        } object;
    };
} json_value_t;

json_value_t  *json_parse(const char *text);
void           json_free(json_value_t *val);

json_value_t  *json_get(json_value_t *obj, const char *key);
const char    *json_get_string(json_value_t *obj, const char *key);
double         json_get_number(json_value_t *obj, const char *key);
bool           json_get_bool(json_value_t *obj, const char *key);
