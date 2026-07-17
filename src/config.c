#include "config.h"
#include "store.h"
#include "schema.h"
#include "backend.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define MAX_PATH 1024
#define MAX_CALLBACKS 8

static void stack_value_cleanup(config_value_t *v);

struct config_t {
    config_schema_t *schema;
    hashmap_t *values;
    char filepath[MAX_PATH];
    int dirty;

    config_changed_cb callbacks[MAX_CALLBACKS];
    void *callback_data[MAX_CALLBACKS];
    int callback_count;
};

config_t *config_new(const char *schema_id, const char *config_dir,
                     const config_schema_entry_t *schema_entries,
                     size_t schema_len) {
    config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) return NULL;

    cfg->schema = schema_create(schema_id, schema_entries, schema_len);
    if (!cfg->schema) { free(cfg); return NULL; }

    cfg->values = hashmap_create();
    if (!cfg->values) { schema_destroy(cfg->schema); free(cfg); return NULL; }

    const char *dir = config_dir ? config_dir : ".";
    snprintf(cfg->filepath, sizeof(cfg->filepath),
             "%s/%s.conf", dir, schema_id);

    if (backend_ensure_dir(dir) != 0) {
        fprintf(stderr, "config: cannot create config directory '%s'\n", dir);
    }

    hashmap_t *loaded = backend_load(cfg->filepath);

    for (size_t i = 0; i < schema_len; i++) {
        const config_schema_entry_t *e = &schema_entries[i];
        config_value_t *stored = loaded ? hashmap_get(loaded, e->key) : NULL;
        if (stored) {
            config_value_t *v = value_clone(stored);
            if (v) hashmap_put(cfg->values, e->key, v);
        } else if (e->default_value) {
            config_value_t v;
            if (value_from_string(&v, e->default_value) == 0) {
                config_value_t *vp = value_clone(&v);
                if (vp) hashmap_put(cfg->values, e->key, vp);
                stack_value_cleanup(&v);
            }
        }
    }

    if (loaded) hashmap_destroy(loaded);
    cfg->dirty = 0;
    return cfg;
}

void config_free(config_t *config) {
    if (!config) return;
    config_sync(config);
    schema_destroy(config->schema);
    hashmap_destroy(config->values);
    free(config);
}

static void stack_value_cleanup(config_value_t *v) {
    if (v->type == CONFIG_TYPE_STRING)
        free(v->data.string_val);
}

static int config_set_value(config_t *config, const char *key,
                            config_value_t *val) {
    if (!config || !key || !val) return -1;

    const config_schema_entry_t *entry = schema_lookup(config->schema, key);
    if (!entry) {
        fprintf(stderr, "config: key '%s' not found in schema\n", key);
        return -1;
    }
    if (!schema_validate_value(entry, val->type)) {
        fprintf(stderr, "config: type mismatch for key '%s'\n", key);
        return -1;
    }

    if (val->type == CONFIG_TYPE_INT || val->type == CONFIG_TYPE_DOUBLE) {
        double v = (val->type == CONFIG_TYPE_INT)
            ? (double)val->data.int_val : val->data.double_val;
        if (entry->range_min != 0.0 || entry->range_max != 0.0) {
            if (v < entry->range_min || v > entry->range_max) {
                fprintf(stderr, "config: value %.15g out of range [%.15g, %.15g] "
                        "for key '%s'\n", v, entry->range_min, entry->range_max, key);
                return -1;
            }
        }
    }

    config_value_t *old = hashmap_get(config->values, key);
    int changed = !old || old->type != val->type;
    if (!changed) {
        switch (val->type) {
        case CONFIG_TYPE_INT:
            changed = old->data.int_val != val->data.int_val; break;
        case CONFIG_TYPE_DOUBLE:
            changed = fabs(old->data.double_val - val->data.double_val) > 1e-15; break;
        case CONFIG_TYPE_STRING:
            changed = strcmp(old->data.string_val, val->data.string_val) != 0; break;
        case CONFIG_TYPE_BOOLEAN:
            changed = old->data.bool_val != val->data.bool_val; break;
        default: break;
        }
    }

    config_value_t *clone = value_clone(val);
    if (!clone) return -1;
    hashmap_put(config->values, key, clone);
    config->dirty = 1;

    if (changed) {
        for (int i = 0; i < config->callback_count; i++)
            config->callbacks[i](config, key, config->callback_data[i]);
    }
    return 0;
}

int config_get_int(config_t *config, const char *key) {
    config_value_t *v = hashmap_get(config->values, key);
    return (v && v->type == CONFIG_TYPE_INT) ? v->data.int_val : 0;
}

double config_get_double(config_t *config, const char *key) {
    config_value_t *v = hashmap_get(config->values, key);
    return (v && v->type == CONFIG_TYPE_DOUBLE) ? v->data.double_val : 0.0;
}

const char *config_get_string(config_t *config, const char *key) {
    config_value_t *v = hashmap_get(config->values, key);
    return (v && v->type == CONFIG_TYPE_STRING) ? v->data.string_val : NULL;
}

int config_get_boolean(config_t *config, const char *key) {
    config_value_t *v = hashmap_get(config->values, key);
    return (v && v->type == CONFIG_TYPE_BOOLEAN) ? v->data.bool_val : 0;
}

int config_set_int(config_t *config, const char *key, int value) {
    config_value_t *v = value_new_int(value);
    if (!v) return -1;
    int rc = config_set_value(config, key, v);
    value_free(v);
    return rc;
}

int config_set_double(config_t *config, const char *key, double value) {
    config_value_t *v = value_new_double(value);
    if (!v) return -1;
    int rc = config_set_value(config, key, v);
    value_free(v);
    return rc;
}

int config_set_string(config_t *config, const char *key, const char *value) {
    config_value_t *v = value_new_string(value);
    if (!v) return -1;
    int rc = config_set_value(config, key, v);
    value_free(v);
    return rc;
}

int config_set_boolean(config_t *config, const char *key, int value) {
    config_value_t *v = value_new_boolean(value);
    if (!v) return -1;
    int rc = config_set_value(config, key, v);
    value_free(v);
    return rc;
}

int config_connect_changed(config_t *config, config_changed_cb callback,
                           void *user_data) {
    if (!config || !callback) return -1;
    if (config->callback_count >= MAX_CALLBACKS) return -1;
    config->callbacks[config->callback_count] = callback;
    config->callback_data[config->callback_count] = user_data;
    config->callback_count++;
    return 0;
}

int config_sync(config_t *config) {
    if (!config || !config->dirty) return 0;
    int rc = backend_save(config->filepath, config->values);
    if (rc == 0) config->dirty = 0;
    return rc;
}

int config_reset(config_t *config, const char *key) {
    if (!config || !key) return -1;
    const config_schema_entry_t *entry = schema_lookup(config->schema, key);
    if (!entry) return -1;

    hashmap_del(config->values, key);
    if (entry->default_value) {
        config_value_t v;
        if (value_from_string(&v, entry->default_value) == 0) {
            config_value_t *vp = value_clone(&v);
            if (vp) hashmap_put(config->values, key, vp);
            stack_value_cleanup(&v);
        }
    }
    config->dirty = 1;
    return 0;
}

int config_reset_all(config_t *config) {
    if (!config) return -1;
    hashmap_destroy(config->values);
    config->values = hashmap_create();
    if (!config->values) return -1;

    const config_schema_entry_t *entries = schema_get_all(config->schema);
    size_t count = schema_get_count(config->schema);
    for (size_t i = 0; i < count; i++) {
        if (entries[i].default_value) {
            config_value_t v;
            if (value_from_string(&v, entries[i].default_value) == 0) {
                config_value_t *vp = value_clone(&v);
                if (vp) hashmap_put(config->values, entries[i].key, vp);
                stack_value_cleanup(&v);
            }
        }
    }
    config->dirty = 1;
    return 0;
}

static void list_cb(const char *key, config_value_t *val, void *user_data) {
    (void)user_data;
    switch (val->type) {
    case CONFIG_TYPE_INT:
        printf("  %s = %d (int)\n", key, val->data.int_val); break;
    case CONFIG_TYPE_DOUBLE:
        printf("  %s = %.15g (double)\n", key, val->data.double_val); break;
    case CONFIG_TYPE_STRING:
        printf("  %s = '%s' (string)\n", key, val->data.string_val); break;
    case CONFIG_TYPE_BOOLEAN:
        printf("  %s = %s (boolean)\n", key, val->data.bool_val ? "true" : "false"); break;
    default: break;
    }
}

void config_list_keys(config_t *config) {
    if (!config) return;
    printf("[%s]\n", schema_get_id(config->schema));
    hashmap_foreach(config->values, list_cb, NULL);
}
