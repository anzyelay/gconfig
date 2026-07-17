#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONFIG_TYPE_INVALID = 0,
    CONFIG_TYPE_INT,
    CONFIG_TYPE_DOUBLE,
    CONFIG_TYPE_STRING,
    CONFIG_TYPE_BOOLEAN
} config_value_type_t;

typedef struct {
    const char *key;
    config_value_type_t type;
    const char *default_value;
    const char *description;
    double range_min;
    double range_max;
} config_schema_entry_t;

typedef struct config_t config_t;
typedef void (*config_changed_cb)(config_t *config, const char *key, void *user_data);

config_t *config_new(const char *schema_id, const char *config_dir,
                     const config_schema_entry_t *schema, size_t schema_len);
config_t *config_new_with_daemon(const char *schema_id, const char *socket_path,
                                 const char *config_dir,
                                 const config_schema_entry_t *schema,
                                 size_t schema_len);
void config_free(config_t *config);

int config_get_int(config_t *config, const char *key);
double config_get_double(config_t *config, const char *key);
const char *config_get_string(config_t *config, const char *key);
int config_get_boolean(config_t *config, const char *key);

int config_set_int(config_t *config, const char *key, int value);
int config_set_double(config_t *config, const char *key, double value);
int config_set_string(config_t *config, const char *key, const char *value);
int config_set_boolean(config_t *config, const char *key, int value);

int config_connect_changed(config_t *config, config_changed_cb callback,
                           void *user_data);

int config_sync(config_t *config);
int config_reset(config_t *config, const char *key);
int config_reset_all(config_t *config);
void config_list_keys(config_t *config);

int config_daemon_is_connected(config_t *config);
int config_daemon_process_events(config_t *config);
int config_daemon_get_fd(config_t *config);

#ifdef __cplusplus
}
#endif

#endif
