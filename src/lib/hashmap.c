#include "hashmap.h"
#include "string.h"
#include <kernel/mm/slab.h>

#define FNV_OFFSET_BASIS 0xcbf29ce484222325ULL
#define FNV_PRIME        0x100000001b3ULL
#define DEFAULT_CAPACITY 16

uint64_t fnv1a_hash(const char *key)
{
    uint64_t h = FNV_OFFSET_BASIS;
    while (*key) {
        h ^= (uint64_t)(uint8_t)*key++;
        h *= FNV_PRIME;
    }
    return h;
}

static size_t next_power_of_2(size_t v)
{
    v--;
    v |= v >> 1;  v |= v >> 2;
    v |= v >> 4;  v |= v >> 8;
    v |= v >> 16; v |= v >> 32;
    return v + 1;
}

hashmap_t *hashmap_create(size_t initial_capacity)
{
    if (initial_capacity < DEFAULT_CAPACITY)
        initial_capacity = DEFAULT_CAPACITY;
    initial_capacity = next_power_of_2(initial_capacity);

    hashmap_t *map = (hashmap_t *)kmalloc(sizeof(hashmap_t));
    if (!map) return NULL;

    map->buckets = (hashmap_entry_t **)kcalloc(initial_capacity, sizeof(hashmap_entry_t *));
    if (!map->buckets) {
        kfree(map);
        return NULL;
    }

    map->capacity       = initial_capacity;
    map->count          = 0;
    map->load_threshold = initial_capacity * 75 / 100;
    return map;
}

static void free_entry(hashmap_entry_t *e)
{
    kfree((void *)e->key);
    kfree(e);
}

void hashmap_destroy(hashmap_t *map)
{
    if (!map) return;
    hashmap_clear(map);
    kfree(map->buckets);
    kfree(map);
}

static char *dup_key(const char *key)
{
    size_t len = strlen(key) + 1;
    char *copy = (char *)kmalloc(len);
    if (copy)
        memcpy(copy, key, len);
    return copy;
}

static int hashmap_resize(hashmap_t *map, size_t new_cap)
{
    hashmap_entry_t **new_buckets = (hashmap_entry_t **)kcalloc(new_cap, sizeof(hashmap_entry_t *));
    if (!new_buckets) return -ENOMEM;

    for (size_t i = 0; i < map->capacity; i++) {
        hashmap_entry_t *e = map->buckets[i];
        while (e) {
            hashmap_entry_t *next = e->next;
            size_t idx = e->hash & (new_cap - 1);
            e->next = new_buckets[idx];
            new_buckets[idx] = e;
            e = next;
        }
    }

    kfree(map->buckets);
    map->buckets        = new_buckets;
    map->capacity       = new_cap;
    map->load_threshold = new_cap * 75 / 100;
    return 0;
}

int hashmap_put(hashmap_t *map, const char *key, void *value)
{
    if (!map || !key) return -EINVAL;

    uint64_t h   = fnv1a_hash(key);
    size_t   idx = h & (map->capacity - 1);

    /* check if key already exists → update */
    for (hashmap_entry_t *e = map->buckets[idx]; e; e = e->next) {
        if (e->hash == h && strcmp(e->key, key) == 0) {
            e->value = value;
            return 0;
        }
    }

    /* auto-resize at 75% load */
    if (map->count >= map->load_threshold) {
        int rc = hashmap_resize(map, map->capacity * 2);
        if (rc) return rc;
        idx = h & (map->capacity - 1);
    }

    hashmap_entry_t *entry = (hashmap_entry_t *)kmalloc(sizeof(hashmap_entry_t));
    if (!entry) return -ENOMEM;

    entry->key   = dup_key(key);
    if (!entry->key) {
        kfree(entry);
        return -ENOMEM;
    }
    entry->value = value;
    entry->hash  = h;
    entry->next  = map->buckets[idx];
    map->buckets[idx] = entry;
    map->count++;
    return 0;
}

void *hashmap_get(hashmap_t *map, const char *key)
{
    if (!map || !key) return NULL;

    uint64_t h   = fnv1a_hash(key);
    size_t   idx = h & (map->capacity - 1);

    for (hashmap_entry_t *e = map->buckets[idx]; e; e = e->next) {
        if (e->hash == h && strcmp(e->key, key) == 0)
            return e->value;
    }
    return NULL;
}

void *hashmap_remove(hashmap_t *map, const char *key)
{
    if (!map || !key) return NULL;

    uint64_t h   = fnv1a_hash(key);
    size_t   idx = h & (map->capacity - 1);

    hashmap_entry_t **prev = &map->buckets[idx];
    for (hashmap_entry_t *e = *prev; e; prev = &e->next, e = e->next) {
        if (e->hash == h && strcmp(e->key, key) == 0) {
            void *val = e->value;
            *prev = e->next;
            free_entry(e);
            map->count--;
            return val;
        }
    }
    return NULL;
}

bool hashmap_contains(hashmap_t *map, const char *key)
{
    if (!map || !key) return false;

    uint64_t h   = fnv1a_hash(key);
    size_t   idx = h & (map->capacity - 1);

    for (hashmap_entry_t *e = map->buckets[idx]; e; e = e->next) {
        if (e->hash == h && strcmp(e->key, key) == 0)
            return true;
    }
    return false;
}

size_t hashmap_size(hashmap_t *map)
{
    return map ? map->count : 0;
}

void hashmap_clear(hashmap_t *map)
{
    if (!map) return;

    for (size_t i = 0; i < map->capacity; i++) {
        hashmap_entry_t *e = map->buckets[i];
        while (e) {
            hashmap_entry_t *next = e->next;
            free_entry(e);
            e = next;
        }
        map->buckets[i] = NULL;
    }
    map->count = 0;
}

void hashmap_foreach(hashmap_t *map, hashmap_iter_fn callback, void *userdata)
{
    if (!map || !callback) return;

    for (size_t i = 0; i < map->capacity; i++) {
        for (hashmap_entry_t *e = map->buckets[i]; e; e = e->next)
            callback(e->key, e->value, userdata);
    }
}
