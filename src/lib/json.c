#include "json.h"
#include "string.h"
#include <kernel/mm/slab.h>

/* ── Lexer helpers ────────────────────────────────────────────────── */

static const char *skip_whitespace(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

static json_value_t *alloc_value(json_type_t type)
{
    json_value_t *v = (json_value_t *)kcalloc(1, sizeof(json_value_t));
    if (v) v->type = type;
    return v;
}

/* ── Forward declarations ─────────────────────────────────────────── */

static json_value_t *parse_value(const char **pp);

/* ── Parse string ─────────────────────────────────────────────────── */

static char *parse_string_raw(const char **pp)
{
    const char *p = *pp;
    if (*p != '"') return NULL;
    p++;

    const char *start = p;
    /* first pass: compute length */
    size_t len = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            if (*p == 'u') { p += 4; len++; }
            else { p++; len++; }
        } else {
            p++;
            len++;
        }
    }
    if (*p != '"') return NULL;

    char *str = (char *)kmalloc(len + 1);
    if (!str) return NULL;

    /* second pass: copy with unescaping */
    p = start;
    size_t i = 0;
    while (*p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"':  str[i++] = '"';  break;
            case '\\': str[i++] = '\\'; break;
            case '/':  str[i++] = '/';  break;
            case 'b':  str[i++] = '\b'; break;
            case 'f':  str[i++] = '\f'; break;
            case 'n':  str[i++] = '\n'; break;
            case 'r':  str[i++] = '\r'; break;
            case 't':  str[i++] = '\t'; break;
            case 'u':
                str[i++] = '?';
                p += 4;
                continue;
            default:
                str[i++] = *p;
                break;
            }
            p++;
        } else {
            str[i++] = *p++;
        }
    }
    str[i] = '\0';
    p++; /* skip closing '"' */
    *pp = p;
    return str;
}

static json_value_t *parse_string(const char **pp)
{
    char *s = parse_string_raw(pp);
    if (!s) return NULL;

    json_value_t *v = alloc_value(JSON_STRING);
    if (!v) { kfree(s); return NULL; }
    v->str_val = s;
    return v;
}

/* ── Parse number (integer + float with exponent) ─────────────────── */

static json_value_t *parse_number(const char **pp)
{
    const char *p = *pp;
    double sign = 1.0;
    if (*p == '-') { sign = -1.0; p++; }

    double integer = 0.0;
    while (*p >= '0' && *p <= '9')
        integer = integer * 10.0 + (*p++ - '0');

    double frac = 0.0;
    if (*p == '.') {
        p++;
        double place = 0.1;
        while (*p >= '0' && *p <= '9') {
            frac += (*p++ - '0') * place;
            place *= 0.1;
        }
    }

    double result = sign * (integer + frac);

    if (*p == 'e' || *p == 'E') {
        p++;
        double esign = 1.0;
        if (*p == '+') p++;
        else if (*p == '-') { esign = -1.0; p++; }

        double exp = 0.0;
        while (*p >= '0' && *p <= '9')
            exp = exp * 10.0 + (*p++ - '0');

        double mul = 1.0;
        for (int i = 0; i < (int)exp; i++)
            mul *= 10.0;
        if (esign < 0)
            result /= mul;
        else
            result *= mul;
    }

    json_value_t *v = alloc_value(JSON_NUMBER);
    if (!v) return NULL;
    v->num_val = result;
    *pp = p;
    return v;
}

/* ── Parse array ──────────────────────────────────────────────────── */

static json_value_t *parse_array(const char **pp)
{
    const char *p = *pp;
    if (*p != '[') return NULL;
    p++;

    json_value_t *arr = alloc_value(JSON_ARRAY);
    if (!arr) return NULL;

    size_t capacity = 8;
    arr->array.items = (json_value_t **)kmalloc(capacity * sizeof(json_value_t *));
    arr->array.count = 0;
    if (!arr->array.items) { kfree(arr); return NULL; }

    p = skip_whitespace(p);
    if (*p == ']') {
        p++;
        *pp = p;
        return arr;
    }

    while (1) {
        p = skip_whitespace(p);
        json_value_t *item = parse_value(&p);
        if (!item) { json_free(arr); return NULL; }

        if (arr->array.count >= capacity) {
            capacity *= 2;
            json_value_t **new_items = (json_value_t **)krealloc(
                arr->array.items, capacity * sizeof(json_value_t *));
            if (!new_items) { json_free(item); json_free(arr); return NULL; }
            arr->array.items = new_items;
        }
        arr->array.items[arr->array.count++] = item;

        p = skip_whitespace(p);
        if (*p == ',') { p++; continue; }
        if (*p == ']') { p++; break; }
        json_free(arr);
        return NULL;
    }

    *pp = p;
    return arr;
}

/* ── Parse object ─────────────────────────────────────────────────── */

static json_value_t *parse_object(const char **pp)
{
    const char *p = *pp;
    if (*p != '{') return NULL;
    p++;

    json_value_t *obj = alloc_value(JSON_OBJECT);
    if (!obj) return NULL;

    size_t capacity = 8;
    obj->object.keys   = (char **)kmalloc(capacity * sizeof(char *));
    obj->object.values = (json_value_t **)kmalloc(capacity * sizeof(json_value_t *));
    obj->object.count  = 0;
    if (!obj->object.keys || !obj->object.values) { json_free(obj); return NULL; }

    p = skip_whitespace(p);
    if (*p == '}') {
        p++;
        *pp = p;
        return obj;
    }

    while (1) {
        p = skip_whitespace(p);
        char *key = parse_string_raw(&p);
        if (!key) { json_free(obj); return NULL; }

        p = skip_whitespace(p);
        if (*p != ':') { kfree(key); json_free(obj); return NULL; }
        p++;

        p = skip_whitespace(p);
        json_value_t *val = parse_value(&p);
        if (!val) { kfree(key); json_free(obj); return NULL; }

        if (obj->object.count >= capacity) {
            capacity *= 2;
            char **new_keys = (char **)krealloc(
                obj->object.keys, capacity * sizeof(char *));
            json_value_t **new_vals = (json_value_t **)krealloc(
                obj->object.values, capacity * sizeof(json_value_t *));
            if (!new_keys || !new_vals) {
                kfree(key); json_free(val); json_free(obj);
                return NULL;
            }
            obj->object.keys   = new_keys;
            obj->object.values = new_vals;
        }

        obj->object.keys[obj->object.count]   = key;
        obj->object.values[obj->object.count]  = val;
        obj->object.count++;

        p = skip_whitespace(p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; break; }
        json_free(obj);
        return NULL;
    }

    *pp = p;
    return obj;
}

/* ── Parse value (dispatch) ───────────────────────────────────────── */

static json_value_t *parse_value(const char **pp)
{
    const char *p = skip_whitespace(*pp);
    *pp = p;

    if (*p == '"')
        return parse_string(pp);

    if (*p == '{')
        return parse_object(pp);

    if (*p == '[')
        return parse_array(pp);

    if (*p == 't' && p[1] == 'r' && p[2] == 'u' && p[3] == 'e') {
        json_value_t *v = alloc_value(JSON_BOOL);
        if (v) v->bool_val = true;
        *pp = p + 4;
        return v;
    }

    if (*p == 'f' && p[1] == 'a' && p[2] == 'l' && p[3] == 's' && p[4] == 'e') {
        json_value_t *v = alloc_value(JSON_BOOL);
        if (v) v->bool_val = false;
        *pp = p + 5;
        return v;
    }

    if (*p == 'n' && p[1] == 'u' && p[2] == 'l' && p[3] == 'l') {
        json_value_t *v = alloc_value(JSON_NULL);
        *pp = p + 4;
        return v;
    }

    if (*p == '-' || (*p >= '0' && *p <= '9'))
        return parse_number(pp);

    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────── */

json_value_t *json_parse(const char *text)
{
    if (!text) return NULL;
    const char *p = text;
    return parse_value(&p);
}

void json_free(json_value_t *val)
{
    if (!val) return;

    switch (val->type) {
    case JSON_STRING:
        kfree(val->str_val);
        break;
    case JSON_ARRAY:
        for (size_t i = 0; i < val->array.count; i++)
            json_free(val->array.items[i]);
        kfree(val->array.items);
        break;
    case JSON_OBJECT:
        for (size_t i = 0; i < val->object.count; i++) {
            kfree(val->object.keys[i]);
            json_free(val->object.values[i]);
        }
        kfree(val->object.keys);
        kfree(val->object.values);
        break;
    default:
        break;
    }
    kfree(val);
}

json_value_t *json_get(json_value_t *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJECT || !key)
        return NULL;

    for (size_t i = 0; i < obj->object.count; i++) {
        if (strcmp(obj->object.keys[i], key) == 0)
            return obj->object.values[i];
    }
    return NULL;
}

const char *json_get_string(json_value_t *obj, const char *key)
{
    json_value_t *v = json_get(obj, key);
    if (v && v->type == JSON_STRING)
        return v->str_val;
    return NULL;
}

double json_get_number(json_value_t *obj, const char *key)
{
    json_value_t *v = json_get(obj, key);
    if (v && v->type == JSON_NUMBER)
        return v->num_val;
    return 0.0;
}

bool json_get_bool(json_value_t *obj, const char *key)
{
    json_value_t *v = json_get(obj, key);
    if (v && v->type == JSON_BOOL)
        return v->bool_val;
    return false;
}
