#ifndef STORE_H
#define STORE_H

#include "config.h"

typedef struct config_value_t {
    config_value_type_t type;
    union {
        int int_val;
        double double_val;
        char *string_val;
        int bool_val;
    } data;
} config_value_t;

config_value_t *value_new_int(int val);
config_value_t *value_new_double(double val);
config_value_t *value_new_string(const char *val);
config_value_t *value_new_boolean(int val);
config_value_t *value_clone(const config_value_t *src);
void value_free(config_value_t *val);
int value_to_string(const config_value_t *val, char *buf, size_t size);
int value_from_string(config_value_t *val, const char *str);

typedef struct hashmap_t hashmap_t;

hashmap_t *hashmap_create(void);
void hashmap_destroy(hashmap_t *map);
int hashmap_put(hashmap_t *map, const char *key, config_value_t *val);
config_value_t *hashmap_get(hashmap_t *map, const char *key);
int hashmap_del(hashmap_t *map, const char *key);
size_t hashmap_size(hashmap_t *map);

typedef void (*hashmap_iter_cb)(const char *key, config_value_t *val,
                                void *user_data);
void hashmap_foreach(const hashmap_t *map, hashmap_iter_cb cb, void *user_data);

#endif
