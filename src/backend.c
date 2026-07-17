#include "backend.h"
#include "store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#define MAX_LINE 4096

int backend_ensure_dir(const char *dirpath) {
    struct stat st;
    if (stat(dirpath, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        return -1;
    }
    if (mkdir(dirpath, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

hashmap_t *backend_load(const char *filepath) {
    hashmap_t *store = hashmap_create();
    if (!store) return NULL;

    FILE *fp = fopen(filepath, "r");
    if (!fp) return store;

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        char *colon = strchr(line, ':');
        if (!colon) continue;

        *colon = '\0';
        char *key = line;
        char *value_str = colon + 1;

        config_value_t *val = calloc(1, sizeof(*val));
        if (!val) continue;
        if (value_from_string(val, value_str) == 0) {
            config_value_t *v = value_clone(val);
            if (v) hashmap_put(store, key, v);
        }
        value_free(val);
    }
    fclose(fp);
    return store;
}

typedef struct {
    FILE *fp;
} save_ctx_t;

static void save_entry_cb(const char *key, config_value_t *val,
                          void *user_data) {
    save_ctx_t *ctx = user_data;
    char buf[MAX_LINE];
    if (value_to_string(val, buf, sizeof(buf)) == 0)
        fprintf(ctx->fp, "%s:%s\n", key, buf);
}

int backend_save(const char *filepath, const hashmap_t *store) {
    if (!store) return -1;

    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", filepath);

    FILE *fp = fopen(tmp_path, "w");
    if (!fp) return -1;

    save_ctx_t ctx = { .fp = fp };
    hashmap_foreach(store, save_entry_cb, &ctx);

    fclose(fp);
    rename(tmp_path, filepath);
    return 0;
}
