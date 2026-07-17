#include "schema.h"
#include <stdlib.h>
#include <string.h>

struct config_schema_t {
    char *schema_id;
    config_schema_entry_t *entries;
    size_t count;
};

config_schema_t *schema_create(const char *schema_id,
                               const config_schema_entry_t *entries,
                               size_t count) {
    config_schema_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->schema_id = strdup(schema_id ? schema_id : "default");
    s->count = count;
    s->entries = calloc(count, sizeof(config_schema_entry_t));
    if (!s->entries) { free(s->schema_id); free(s); return NULL; }

    for (size_t i = 0; i < count; i++) {
        s->entries[i].key = strdup(entries[i].key);
        s->entries[i].type = entries[i].type;
        s->entries[i].default_value = entries[i].default_value
            ? strdup(entries[i].default_value) : NULL;
        s->entries[i].description = entries[i].description
            ? strdup(entries[i].description) : NULL;
        s->entries[i].range_min = entries[i].range_min;
        s->entries[i].range_max = entries[i].range_max;
    }
    return s;
}

void schema_destroy(config_schema_t *schema) {
    if (!schema) return;
    for (size_t i = 0; i < schema->count; i++) {
        free((void *)schema->entries[i].key);
        free((void *)schema->entries[i].default_value);
        free((void *)schema->entries[i].description);
    }
    free(schema->entries);
    free(schema->schema_id);
    free(schema);
}

const config_schema_entry_t *schema_lookup(const config_schema_t *schema,
                                           const char *key) {
    if (!schema || !key) return NULL;
    for (size_t i = 0; i < schema->count; i++) {
        if (strcmp(schema->entries[i].key, key) == 0)
            return &schema->entries[i];
    }
    return NULL;
}

const char *schema_get_id(const config_schema_t *schema) {
    return schema ? schema->schema_id : NULL;
}

size_t schema_get_count(const config_schema_t *schema) {
    return schema ? schema->count : 0;
}

const config_schema_entry_t *schema_get_all(const config_schema_t *schema) {
    return schema ? schema->entries : NULL;
}

int schema_validate_value(const config_schema_entry_t *entry,
                          config_value_type_t type) {
    if (!entry) return 0;
    return entry->type == type ? 1 : 0;
}
