#include "config.h"
#include "store.h"
#include "schema.h"
#include "backend.h"
#include "ipc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>

#define MAX_PATH 1024
#define MAX_CALLBACKS 8
#define MAX_SOCK_PATH 256

static void stack_value_cleanup(config_value_t *v);

struct config_t {
    config_schema_t *schema;
    hashmap_t *values;
    char filepath[MAX_PATH];
    int dirty;

    config_changed_cb callbacks[MAX_CALLBACKS];
    void *callback_data[MAX_CALLBACKS];
    int callback_count;

    int use_daemon;
    config_ipc_t *ipc;
    char sock_path[MAX_SOCK_PATH];
};

static int try_connect_daemon(config_t *cfg) {
    if (cfg->ipc) return 0;
    cfg->ipc = config_ipc_connect(cfg->sock_path);
    return cfg->ipc ? 0 : -1;
}

static int daemon_send_recv(config_t *cfg, const char *cmd_fmt, ...) {
    if (!cfg->ipc && try_connect_daemon(cfg) != 0) return -1;

    char cmdbuf[4096];
    va_list ap;
    va_start(ap, cmd_fmt);
    vsnprintf(cmdbuf, sizeof(cmdbuf), cmd_fmt, ap);
    va_end(ap);

    if (config_ipc_send_fmt(cfg->ipc, "%s", cmdbuf) != 0) {
        config_ipc_disconnect(cfg->ipc);
        cfg->ipc = NULL;
        return -1;
    }
    return 0;
}

static int daemon_recv_line(config_t *cfg, char *buf, size_t size) {
    if (!cfg->ipc) return -1;
    if (config_ipc_recv_line(cfg->ipc, buf, size) != 0) {
        config_ipc_disconnect(cfg->ipc);
        cfg->ipc = NULL;
        return -1;
    }
    return 0;
}

static int auto_start_daemon(const char *sock_path, const char *config_dir) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        char dir_arg[1024];
        snprintf(dir_arg, sizeof(dir_arg), "%s", config_dir);
        execlp("configd", "configd", "-s", sock_path, "-d", dir_arg, NULL);
        _exit(1);
    }
    for (int i = 0; i < 20; i++) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L };
        nanosleep(&ts, NULL);
        int fd = -1;
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) continue;
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            close(fd);
            int status;
            waitpid(pid, &status, WNOHANG);
            return 0;
        }
        close(fd);
    }
    return -1;
}

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

config_t *config_new_with_daemon(const char *schema_id,
                                 const char *socket_path,
                                 const char *config_dir,
                                 const config_schema_entry_t *schema_entries,
                                 size_t schema_len) {
    config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) return NULL;

    cfg->schema = schema_create(schema_id, schema_entries, schema_len);
    if (!cfg->schema) { free(cfg); return NULL; }

    cfg->values = hashmap_create();
    if (!cfg->values) { schema_destroy(cfg->schema); free(cfg); return NULL; }

    cfg->use_daemon = 1;
    const char *sp = socket_path ? socket_path : "/tmp/configd.sock";
    snprintf(cfg->sock_path, sizeof(cfg->sock_path), "%s", sp);

    const char *cd = config_dir ? config_dir : ".";
    snprintf(cfg->filepath, sizeof(cfg->filepath), "%s/%s.conf", cd, schema_id);

    if (try_connect_daemon(cfg) != 0) {
        auto_start_daemon(cfg->sock_path, cd);
        try_connect_daemon(cfg);
    }

    if (!cfg->ipc) {
        fprintf(stderr, "config: cannot connect to daemon at %s\n", cfg->sock_path);
    }

    if (cfg->ipc) {
        char rbuf[4096];
        daemon_send_recv(cfg, "SUBSCRIBE %s", schema_id);
        daemon_recv_line(cfg, rbuf, sizeof(rbuf));

        daemon_send_recv(cfg, "LIST %s", schema_id);
        daemon_recv_line(cfg, rbuf, sizeof(rbuf));

        int count = 0;
        if (strncmp(rbuf, "OK ", 3) == 0) count = atoi(rbuf + 3);

        for (int i = 0; i < count; i++) {
            daemon_recv_line(cfg, rbuf, sizeof(rbuf));
            char *colon = strchr(rbuf, ':');
            if (!colon) continue;
            *colon = '\0';
            config_value_t val;
            if (value_from_string(&val, colon + 1) == 0) {
                config_value_t *vp = value_clone(&val);
                if (vp) hashmap_put(cfg->values, rbuf, vp);
                stack_value_cleanup(&val);
            }
        }
    }

    for (size_t i = 0; i < schema_len; i++) {
        const config_schema_entry_t *e = &schema_entries[i];
        config_value_t *stored = hashmap_get(cfg->values, e->key);
        if (!stored && e->default_value) {
            config_value_t v;
            if (value_from_string(&v, e->default_value) == 0) {
                config_value_t *vp = value_clone(&v);
                if (vp) {
                    hashmap_put(cfg->values, e->key, vp);
                    if (cfg->ipc) {
                        char vbuf[256];
                        value_to_string(vp, vbuf, sizeof(vbuf));
                        daemon_send_recv(cfg, "SET %s %s %s", schema_id, e->key, vbuf);
                        char rbuf2[128];
                        daemon_recv_line(cfg, rbuf2, sizeof(rbuf2));
                    }
                }
                stack_value_cleanup(&v);
            }
        }
    }

    if (cfg->ipc) {
        char rbuf3[128];
        daemon_send_recv(cfg, "SYNC %s", schema_id);
        daemon_recv_line(cfg, rbuf3, sizeof(rbuf3));
    }

    return cfg;
}

void config_free(config_t *config) {
    if (!config) return;
    config_sync(config);
    if (config->ipc) {
        daemon_send_recv(config, "UNSUBSCRIBE %s",
                         schema_get_id(config->schema));
        char buf[64];
        daemon_recv_line(config, buf, sizeof(buf));
        config_ipc_disconnect(config->ipc);
    }
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

    if (config->use_daemon) {
        char vbuf[256];
        value_to_string(val, vbuf, sizeof(vbuf));
        if (daemon_send_recv(config, "SET %s %s %s",
                             schema_get_id(config->schema), key, vbuf) == 0) {
            char rbuf[128];
            daemon_recv_line(config, rbuf, sizeof(rbuf));
            if (strncmp(rbuf, "OK", 2) != 0) {
                fprintf(stderr, "config: daemon SET failed: %s\n", rbuf);
                return -1;
            }
        } else {
            return -1;
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

static int daemon_get_value(config_t *config, const char *key, config_value_type_t expected,
                            void *out) {
    if (daemon_send_recv(config, "GET %s %s",
                         schema_get_id(config->schema), key) != 0) {
        return -1;
    }
    char rbuf[512];
    if (daemon_recv_line(config, rbuf, sizeof(rbuf)) != 0) return -1;

    if (strncmp(rbuf, "OK ", 3) != 0) return -1;

    config_value_t val;
    if (value_from_string(&val, rbuf + 3) != 0) return -1;
    if (val.type != expected) {
        stack_value_cleanup(&val);
        return -1;
    }

    config_value_t *clone = value_clone(&val);
    if (clone) hashmap_put(config->values, key, clone);
    stack_value_cleanup(&val);

    switch (expected) {
    case CONFIG_TYPE_INT:    *(int *)out = clone->data.int_val; break;
    case CONFIG_TYPE_DOUBLE: *(double *)out = clone->data.double_val; break;
    case CONFIG_TYPE_STRING: *(const char **)out = clone->data.string_val; break;
    case CONFIG_TYPE_BOOLEAN: *(int *)out = clone->data.bool_val; break;
    default: return -1;
    }
    return 0;
}

int config_get_int(config_t *config, const char *key) {
    if (config->use_daemon) {
        int val = 0;
        if (daemon_get_value(config, key, CONFIG_TYPE_INT, &val) == 0)
            return val;
        config_value_t *v = hashmap_get(config->values, key);
        return (v && v->type == CONFIG_TYPE_INT) ? v->data.int_val : 0;
    }
    config_value_t *v = hashmap_get(config->values, key);
    return (v && v->type == CONFIG_TYPE_INT) ? v->data.int_val : 0;
}

double config_get_double(config_t *config, const char *key) {
    if (config->use_daemon) {
        double val = 0.0;
        if (daemon_get_value(config, key, CONFIG_TYPE_DOUBLE, &val) == 0)
            return val;
        config_value_t *v = hashmap_get(config->values, key);
        return (v && v->type == CONFIG_TYPE_DOUBLE) ? v->data.double_val : 0.0;
    }
    config_value_t *v = hashmap_get(config->values, key);
    return (v && v->type == CONFIG_TYPE_DOUBLE) ? v->data.double_val : 0.0;
}

const char *config_get_string(config_t *config, const char *key) {
    if (config->use_daemon) {
        const char *val = NULL;
        if (daemon_get_value(config, key, CONFIG_TYPE_STRING, &val) == 0)
            return val;
        config_value_t *v = hashmap_get(config->values, key);
        return (v && v->type == CONFIG_TYPE_STRING) ? v->data.string_val : NULL;
    }
    config_value_t *v = hashmap_get(config->values, key);
    return (v && v->type == CONFIG_TYPE_STRING) ? v->data.string_val : NULL;
}

int config_get_boolean(config_t *config, const char *key) {
    if (config->use_daemon) {
        int val = 0;
        if (daemon_get_value(config, key, CONFIG_TYPE_BOOLEAN, &val) == 0)
            return val;
        config_value_t *v = hashmap_get(config->values, key);
        return (v && v->type == CONFIG_TYPE_BOOLEAN) ? v->data.bool_val : 0;
    }
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
    if (!config) return 0;
    if (config->use_daemon) {
        if (!config->ipc) return -1;
        daemon_send_recv(config, "SYNC %s", schema_get_id(config->schema));
        char buf[64];
        daemon_recv_line(config, buf, sizeof(buf));
        return (strncmp(buf, "OK", 2) == 0) ? 0 : -1;
    }
    if (!config->dirty) return 0;
    int rc = backend_save(config->filepath, config->values);
    if (rc == 0) config->dirty = 0;
    return rc;
}

int config_reset(config_t *config, const char *key) {
    if (!config || !key) return -1;
    const config_schema_entry_t *entry = schema_lookup(config->schema, key);
    if (!entry) return -1;

    if (config->use_daemon) {
        daemon_send_recv(config, "RESET %s %s",
                         schema_get_id(config->schema), key);
        char buf[64];
        daemon_recv_line(config, buf, sizeof(buf));
    }

    hashmap_del(config->values, key);
    if (entry->default_value) {
        config_value_t v;
        if (value_from_string(&v, entry->default_value) == 0) {
            config_value_t *vp = value_clone(&v);
            if (vp) {
                hashmap_put(config->values, key, vp);
                if (config->use_daemon) {
                    char vbuf[256];
                    value_to_string(vp, vbuf, sizeof(vbuf));
                    daemon_send_recv(config, "SET %s %s %s",
                                     schema_get_id(config->schema), key, vbuf);
                    char rbuf[64];
                    daemon_recv_line(config, rbuf, sizeof(rbuf));
                }
            }
            stack_value_cleanup(&v);
        }
    }
    config->dirty = 1;
    return 0;
}

int config_reset_all(config_t *config) {
    if (!config) return -1;

    if (config->use_daemon) {
        daemon_send_recv(config, "RESET_ALL %s", schema_get_id(config->schema));
        char buf[64];
        daemon_recv_line(config, buf, sizeof(buf));
    }

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
                if (vp) {
                    hashmap_put(config->values, entries[i].key, vp);
                    if (config->use_daemon) {
                        char vbuf[256];
                        value_to_string(vp, vbuf, sizeof(vbuf));
                        daemon_send_recv(config, "SET %s %s %s",
                                         schema_get_id(config->schema),
                                         entries[i].key, vbuf);
                        char rbuf[64];
                        daemon_recv_line(config, rbuf, sizeof(rbuf));
                    }
                }
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

int config_daemon_is_connected(config_t *config) {
    return (config && config->ipc) ? 1 : 0;
}

int config_daemon_get_fd(config_t *config) {
    return config ? config_ipc_get_fd(config->ipc) : -1;
}

int config_daemon_process_events(config_t *config) {
    if (!config || !config->ipc) return 0;

    int count = 0;
    int sockfd = config_ipc_get_fd(config->ipc);
    if (sockfd < 0) return 0;

    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0) return 0;
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    char buf[MAX_PATH];
    for (;;) {
        if (config_ipc_recv_line(config->ipc, buf, sizeof(buf)) != 0)
            break;

        if (strncmp(buf, "NOTIFY ", 7) != 0) continue;

        char *saveptr;
        char *line = buf;
        char *cmd = strtok_r(line, " ", &saveptr);
        char *ns = strtok_r(NULL, " ", &saveptr);
        char *key = strtok_r(NULL, " ", &saveptr);
        char *tval = strtok_r(NULL, " ", &saveptr);

        if (!cmd || !ns || !key) continue;
        if (strcmp(ns, schema_get_id(config->schema)) != 0) continue;

        config_value_t *old = hashmap_get(config->values, key);

        if (tval && strcmp(tval, "-") != 0) {
            config_value_t parsed;
            if (value_from_string(&parsed, tval) == 0) {
                config_value_t *vp = value_clone(&parsed);
                if (vp) hashmap_put(config->values, key, vp);
                stack_value_cleanup(&parsed);
            }
        } else {
            hashmap_del(config->values, key);
            const config_schema_entry_t *entry = schema_lookup(config->schema, key);
            if (entry && entry->default_value) {
                config_value_t v;
                if (value_from_string(&v, entry->default_value) == 0) {
                    config_value_t *vp = value_clone(&v);
                    if (vp) hashmap_put(config->values, key, vp);
                    stack_value_cleanup(&v);
                }
            }
        }

        config_value_t *newv = hashmap_get(config->values, key);
        int changed = (old == NULL) != (newv == NULL);
        if (!changed && old && newv) {
            if (old->type != newv->type) {
                changed = 1;
            } else {
                switch (old->type) {
                case CONFIG_TYPE_INT:
                    changed = old->data.int_val != newv->data.int_val; break;
                case CONFIG_TYPE_DOUBLE:
                    changed = fabs(old->data.double_val - newv->data.double_val) > 1e-15; break;
                case CONFIG_TYPE_STRING:
                    changed = strcmp(old->data.string_val, newv->data.string_val) != 0; break;
                case CONFIG_TYPE_BOOLEAN:
                    changed = old->data.bool_val != newv->data.bool_val; break;
                default: break;
                }
            }
        }

        if (changed) {
            for (int i = 0; i < config->callback_count; i++)
                config->callbacks[i](config, key, config->callback_data[i]);
        }
        count++;
    }

    fcntl(sockfd, F_SETFL, flags & ~O_NONBLOCK);
    return count;
}
