#include "store.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

config_value_t *value_new_int(int val) {
    config_value_t *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->type = CONFIG_TYPE_INT;
    v->data.int_val = val;
    return v;
}

config_value_t *value_new_double(double val) {
    config_value_t *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->type = CONFIG_TYPE_DOUBLE;
    v->data.double_val = val;
    return v;
}

config_value_t *value_new_string(const char *val) {
    config_value_t *v = calloc(1, sizeof(*v));
    if (!v || !val) return v;
    v->type = CONFIG_TYPE_STRING;
    v->data.string_val = strdup(val);
    return v;
}

config_value_t *value_new_boolean(int val) {
    config_value_t *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->type = CONFIG_TYPE_BOOLEAN;
    v->data.bool_val = val ? 1 : 0;
    return v;
}

config_value_t *value_clone(const config_value_t *src) {
    if (!src) return NULL;
    switch (src->type) {
    case CONFIG_TYPE_INT:    return value_new_int(src->data.int_val);
    case CONFIG_TYPE_DOUBLE: return value_new_double(src->data.double_val);
    case CONFIG_TYPE_STRING: return value_new_string(src->data.string_val);
    case CONFIG_TYPE_BOOLEAN:return value_new_boolean(src->data.bool_val);
    default: return NULL;
    }
}

void value_free(config_value_t *val) {
    if (!val) return;
    if (val->type == CONFIG_TYPE_STRING)
        free(val->data.string_val);
    free(val);
}

int value_to_string(const config_value_t *val, char *buf, size_t size) {
    if (!val || !buf || size == 0) return -1;
    switch (val->type) {
    case CONFIG_TYPE_INT:
        snprintf(buf, size, "i:%d", val->data.int_val);
        break;
    case CONFIG_TYPE_DOUBLE:
        snprintf(buf, size, "d:%.15g", val->data.double_val);
        break;
    case CONFIG_TYPE_STRING:
        snprintf(buf, size, "s:%s", val->data.string_val);
        break;
    case CONFIG_TYPE_BOOLEAN:
        snprintf(buf, size, "b:%d", val->data.bool_val);
        break;
    default:
        return -1;
    }
    return 0;
}

int value_from_string(config_value_t *val, const char *str) {
    if (!val || !str || strlen(str) < 2 || str[1] != ':') return -1;
    switch (str[0]) {
    case 'i':
        val->type = CONFIG_TYPE_INT;
        val->data.int_val = atoi(str + 2);
        break;
    case 'd':
        val->type = CONFIG_TYPE_DOUBLE;
        val->data.double_val = atof(str + 2);
        break;
    case 's':
        val->type = CONFIG_TYPE_STRING;
        val->data.string_val = strdup(str + 2);
        break;
    case 'b':
        val->type = CONFIG_TYPE_BOOLEAN;
        val->data.bool_val = atoi(str + 2) ? 1 : 0;
        break;
    default:
        return -1;
    }
    return 0;
}

#define HASHMAP_INIT_CAP 16
#define HASHMAP_LOAD_FACTOR 75

typedef struct hash_entry_t {
    char *key;
    config_value_t *value;
    struct hash_entry_t *next;
} hash_entry_t;

struct hashmap_t {
    hash_entry_t **buckets;
    size_t capacity;
    size_t count;
};

hashmap_t *hashmap_create(void) {
    hashmap_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->capacity = HASHMAP_INIT_CAP;
    m->buckets = calloc(m->capacity, sizeof(hash_entry_t *));
    if (!m->buckets) { free(m); return NULL; }
    return m;
}

static unsigned long hash_str(const char *s) {
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*s++))
        h = ((h << 5) + h) + c;
    return h;
}

static void hashmap_resize(hashmap_t *map) {
    size_t old_cap = map->capacity;
    hash_entry_t **old = map->buckets;

    map->capacity *= 2;
    map->buckets = calloc(map->capacity, sizeof(hash_entry_t *));
    map->count = 0;

    for (size_t i = 0; i < old_cap; i++) {
        hash_entry_t *e = old[i];
        while (e) {
            hash_entry_t *next = e->next;
            size_t idx = hash_str(e->key) % map->capacity;
            e->next = map->buckets[idx];
            map->buckets[idx] = e;
            map->count++;
            e = next;
        }
    }
    free(old);
}

int hashmap_put(hashmap_t *map, const char *key, config_value_t *val) {
    if (!map || !key || !val) return -1;

    if (map->count * 100 / map->capacity >= HASHMAP_LOAD_FACTOR)
        hashmap_resize(map);

    size_t idx = hash_str(key) % map->capacity;
    hash_entry_t *e = map->buckets[idx];
    while (e) {
        if (strcmp(e->key, key) == 0) {
            value_free(e->value);
            e->value = val;
            return 0;
        }
        e = e->next;
    }

    e = malloc(sizeof(*e));
    if (!e) return -1;
    e->key = strdup(key);
    e->value = val;
    e->next = map->buckets[idx];
    map->buckets[idx] = e;
    map->count++;
    return 0;
}

config_value_t *hashmap_get(hashmap_t *map, const char *key) {
    if (!map || !key) return NULL;
    size_t idx = hash_str(key) % map->capacity;
    hash_entry_t *e = map->buckets[idx];
    while (e) {
        if (strcmp(e->key, key) == 0)
            return e->value;
        e = e->next;
    }
    return NULL;
}

int hashmap_del(hashmap_t *map, const char *key) {
    if (!map || !key) return -1;
    size_t idx = hash_str(key) % map->capacity;
    hash_entry_t **pp = &map->buckets[idx];
    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            hash_entry_t *victim = *pp;
            *pp = victim->next;
            free(victim->key);
            value_free(victim->value);
            free(victim);
            map->count--;
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

size_t hashmap_size(hashmap_t *map) {
    return map ? map->count : 0;
}

void hashmap_foreach(const hashmap_t *map, hashmap_iter_cb cb, void *user_data) {
    if (!map || !cb) return;
    for (size_t i = 0; i < map->capacity; i++) {
        hash_entry_t *e = map->buckets[i];
        while (e) {
            cb(e->key, e->value, user_data);
            e = e->next;
        }
    }
}

void hashmap_destroy(hashmap_t *map) {
    if (!map) return;
    for (size_t i = 0; i < map->capacity; i++) {
        hash_entry_t *e = map->buckets[i];
        while (e) {
            hash_entry_t *next = e->next;
            free(e->key);
            value_free(e->value);
            free(e);
            e = next;
        }
    }
    free(map->buckets);
    free(map);
}
