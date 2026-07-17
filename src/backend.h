#ifndef BACKEND_H
#define BACKEND_H

#include "config.h"

typedef struct hashmap_t hashmap_t;

hashmap_t *backend_load(const char *filepath);
int backend_save(const char *filepath, const hashmap_t *store);
int backend_ensure_dir(const char *dirpath);

#endif
