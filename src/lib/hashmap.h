#pragma once

#include <aevos/types.h>

typedef struct hashmap_entry {
    const char          *key;
    void                *value;
    uint64_t             hash;
    struct hashmap_entry *next;
} hashmap_entry_t;

typedef void (*hashmap_iter_fn)(const char *key, void *value, void *userdata);

typedef struct {
    hashmap_entry_t **buckets;
    size_t            count;
    size_t            capacity;
    size_t            load_threshold;  /* capacity * 75 / 100 */
} hashmap_t;

hashmap_t *hashmap_create(size_t initial_capacity);
void       hashmap_destroy(hashmap_t *map);

int        hashmap_put(hashmap_t *map, const char *key, void *value);
void      *hashmap_get(hashmap_t *map, const char *key);
void      *hashmap_remove(hashmap_t *map, const char *key);
bool       hashmap_contains(hashmap_t *map, const char *key);
size_t     hashmap_size(hashmap_t *map);
void       hashmap_clear(hashmap_t *map);
void       hashmap_foreach(hashmap_t *map, hashmap_iter_fn callback, void *userdata);

uint64_t   fnv1a_hash(const char *key);
