#ifndef SCHEMA_H
#define SCHEMA_H

#include "config.h"

typedef struct config_schema_t config_schema_t;

config_schema_t *schema_create(const char *schema_id,
                               const config_schema_entry_t *entries,
                               size_t count);
void schema_destroy(config_schema_t *schema);

const config_schema_entry_t *schema_lookup(const config_schema_t *schema,
                                           const char *key);
const char *schema_get_id(const config_schema_t *schema);
size_t schema_get_count(const config_schema_t *schema);
const config_schema_entry_t *schema_get_all(const config_schema_t *schema);

int schema_validate_value(const config_schema_entry_t *entry,
                          config_value_type_t type);

#endif
