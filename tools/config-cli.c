#include "../src/ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>

#define SOCK_DEFAULT "/tmp/configd.sock"
#define MAX_LINE 4096

static const char *g_socket_path = SOCK_DEFAULT;
static const char *g_value_type = NULL;
static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig) { (void)sig; g_running = 0; }

static void usage(const char *prog) {
    printf("Usage: %s [OPTIONS] COMMAND [ARGS...]\n\n", prog);
    printf("Options:\n");
    printf("  -s <path>   Daemon socket path (default: %s)\n", SOCK_DEFAULT);
    printf("  -t <type>   Value type: int, double, string, boolean (for set command)\n\n");
    printf("Commands:\n");
    printf("  get <schema> <key>             Get a configuration value\n");
    printf("  set <schema> <key> <value>     Set a configuration value\n");
    printf("  reset <schema> <key>           Reset a key to its default\n");
    printf("  reset-all <schema>             Reset all keys in a schema\n");
    printf("  list-keys <schema>             List all keys and values\n");
    printf("  list-schemas                   List all registered schemas\n");
    printf("  monitor <schema> [key]         Monitor changes in real-time\n");
    printf("  help                           Show this help\n\n");
    printf("Examples:\n");
    printf("  %s get myapp theme\n", prog);
    printf("  %s set myapp window-width 1920 -t int\n", prog);
    printf("  %s set myapp theme dark -t string\n", prog);
    printf("  %s monitor myapp\n", prog);
}

static int connect_to_daemon(config_ipc_t **ipc) {
    *ipc = config_ipc_connect(g_socket_path);
    if (!*ipc) {
        fprintf(stderr, "config-cli: cannot connect to daemon at %s\n"
                "  Make sure configd is running.\n", g_socket_path);
        return -1;
    }
    return 0;
}

static int send_cmd(config_ipc_t *ipc, const char *fmt, ...) {
    char buf[MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return config_ipc_send_fmt(ipc, "%s", buf);
}

static int recv_line(config_ipc_t *ipc, char *buf, size_t size) {
    return config_ipc_recv_line(ipc, buf, size);
}

static const char *type_to_str(char c) {
    switch (c) {
    case 'i': return "int";
    case 'd': return "double";
    case 's': return "string";
    case 'b': return "boolean";
    default: return "unknown";
    }
}

static void print_value(const char *type_val) {
    if (!type_val || strlen(type_val) < 2) {
        printf("(none)\n");
        return;
    }
    char t = type_val[0];
    const char *v = type_val + 2;
    switch (t) {
    case 'i': printf("%s\n", v); break;
    case 'd': printf("%s\n", v); break;
    case 's': printf("'%s'\n", v); break;
    case 'b': printf("%s\n", strcmp(v, "1") == 0 ? "true" : "false"); break;
    default: printf("%s\n", v); break;
    }
}

static int auto_detect_type(const char *value) {
    if (!value || !value[0]) return 's';
    if (strcmp(value, "true") == 0 || strcmp(value, "false") == 0) return 'b';
    char *end;
    strtol(value, &end, 10);
    if (*end == '\0') return 'i';
    strtod(value, &end);
    if (*end == '\0') return 'd';
    return 's';
}

static int cmd_get(config_ipc_t *ipc, const char *schema, const char *key) {
    send_cmd(ipc, "GET %s %s", schema, key);
    char buf[MAX_LINE];
    if (recv_line(ipc, buf, sizeof(buf)) != 0) {
        fprintf(stderr, "config-cli: connection lost\n");
        return 1;
    }
    if (strncmp(buf, "OK ", 3) == 0) {
        print_value(buf + 3);
        return 0;
    }
    fprintf(stderr, "config-cli: %s\n", buf);
    return 1;
}

static int cmd_set(config_ipc_t *ipc, const char *schema, const char *key,
                   const char *value) {
    char t;
    if (g_value_type) {
        if (strcmp(g_value_type, "int") == 0) t = 'i';
        else if (strcmp(g_value_type, "double") == 0) t = 'd';
        else if (strcmp(g_value_type, "string") == 0) t = 's';
        else if (strcmp(g_value_type, "boolean") == 0) t = 'b';
        else {
            fprintf(stderr, "config-cli: unknown type '%s'\n", g_value_type);
            return 1;
        }
    } else {
        t = auto_detect_type(value);
    }

    char tval[MAX_LINE];
    if (t == 'b')
        snprintf(tval, sizeof(tval), "b:%d",
                 (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) ? 1 : 0);
    else
        snprintf(tval, sizeof(tval), "%c:%s", t, value);

    send_cmd(ipc, "SET %s %s %s", schema, key, tval);
    char buf[MAX_LINE];
    if (recv_line(ipc, buf, sizeof(buf)) != 0) {
        fprintf(stderr, "config-cli: connection lost\n");
        return 1;
    }
    if (strcmp(buf, "OK") == 0) return 0;
    fprintf(stderr, "config-cli: %s\n", buf);
    return 1;
}

static int cmd_reset(config_ipc_t *ipc, const char *schema, const char *key) {
    send_cmd(ipc, "RESET %s %s", schema, key);
    char buf[MAX_LINE];
    if (recv_line(ipc, buf, sizeof(buf)) != 0) {
        fprintf(stderr, "config-cli: connection lost\n");
        return 1;
    }
    if (strcmp(buf, "OK") == 0) return 0;
    fprintf(stderr, "config-cli: %s\n", buf);
    return 1;
}

static int cmd_reset_all(config_ipc_t *ipc, const char *schema) {
    send_cmd(ipc, "RESET_ALL %s", schema);
    char buf[MAX_LINE];
    if (recv_line(ipc, buf, sizeof(buf)) != 0) {
        fprintf(stderr, "config-cli: connection lost\n");
        return 1;
    }
    if (strcmp(buf, "OK") == 0) return 0;
    fprintf(stderr, "config-cli: %s\n", buf);
    return 1;
}

static int cmd_list_keys(config_ipc_t *ipc, const char *schema) {
    send_cmd(ipc, "LIST %s", schema);
    char buf[MAX_LINE];
    if (recv_line(ipc, buf, sizeof(buf)) != 0) {
        fprintf(stderr, "config-cli: connection lost\n");
        return 1;
    }
    if (strncmp(buf, "OK ", 3) != 0) {
        fprintf(stderr, "config-cli: %s\n", buf);
        return 1;
    }
    int count = atoi(buf + 3);
    if (count == 0) {
        printf("(no keys)\n");
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (recv_line(ipc, buf, sizeof(buf)) != 0) {
            fprintf(stderr, "config-cli: connection lost\n");
            return 1;
        }
        char *colon1 = strchr(buf, ':');
        if (!colon1) continue;
        *colon1 = '\0';
        char *keyname = buf;
        char *full_tval = colon1 + 1;
        char type_str[16] = "";
        if (full_tval && full_tval[0])
            snprintf(type_str, sizeof(type_str), "%s", type_to_str(full_tval[0]));
        printf("%-24s %-8s ", keyname, type_str);
        print_value(full_tval);
    }
    return 0;
}

static int cmd_list_schemas(config_ipc_t *ipc) {
    send_cmd(ipc, "LIST_NS");
    char buf[MAX_LINE];
    if (recv_line(ipc, buf, sizeof(buf)) != 0) {
        fprintf(stderr, "config-cli: connection lost\n");
        return 1;
    }
    if (strncmp(buf, "OK ", 3) != 0) {
        fprintf(stderr, "config-cli: %s\n", buf);
        return 1;
    }
    int count = atoi(buf + 3);
    if (count == 0) {
        printf("(no schemas)\n");
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (recv_line(ipc, buf, sizeof(buf)) != 0) {
            fprintf(stderr, "config-cli: connection lost\n");
            return 1;
        }
        printf("%s\n", buf);
    }
    return 0;
}

static int cmd_monitor(config_ipc_t *ipc, const char *schema, const char *key_filter) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    send_cmd(ipc, "SUBSCRIBE %s", schema);
    char buf[MAX_LINE];
    recv_line(ipc, buf, sizeof(buf));

    printf("Monitoring '%s'", schema);
    if (key_filter) printf(" (key: '%s')", key_filter);
    printf(" - press Ctrl+C to stop\n");

    int fd = config_ipc_get_fd(ipc);
    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int n = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) break;
            break;
        }
        if (n == 0) continue;

        if (recv_line(ipc, buf, sizeof(buf)) != 0) break;
        if (strncmp(buf, "NOTIFY ", 7) != 0) continue;

        char *saveptr;
        char *line = buf;
        strtok_r(line, " ", &saveptr);
        char *ns = strtok_r(NULL, " ", &saveptr);
        char *key = strtok_r(NULL, " ", &saveptr);
        char *tval = saveptr;
        while (*tval == ' ') tval++;

        if (!ns || !key || !tval) continue;
        if (strcmp(ns, schema) != 0) continue;
        if (key_filter && strcmp(key, key_filter) != 0) continue;

        if (strcmp(tval, "-") == 0)
            printf("%s: (reset)\n", key);
        else {
            printf("%s: ", key);
            print_value(tval);
        }
        fflush(stdout);
    }

    send_cmd(ipc, "UNSUBSCRIBE %s", schema);
    recv_line(ipc, buf, sizeof(buf));
    return 0;
}

int main(int argc, char *argv[]) {
    const char *cmd = NULL;
    int arg_start = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            g_socket_path = argv[++i];
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            g_value_type = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            cmd = argv[i];
            arg_start = i;
            break;
        }
    }

    if (!cmd) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(cmd, "help") == 0) {
        usage(argv[0]);
        return 0;
    }

    config_ipc_t *ipc = NULL;
    if (connect_to_daemon(&ipc) != 0)
        return 1;

    int rc = 0;
    int args_after = argc - arg_start - 1;

    if (strcmp(cmd, "get") == 0) {
        if (args_after < 2) { fprintf(stderr, "Usage: %s get <schema> <key>\n", argv[0]); rc = 1; }
        else rc = cmd_get(ipc, argv[arg_start + 1], argv[arg_start + 2]);
    } else if (strcmp(cmd, "set") == 0) {
        if (args_after < 3) { fprintf(stderr, "Usage: %s set <schema> <key> <value> [-t type]\n", argv[0]); rc = 1; }
        else rc = cmd_set(ipc, argv[arg_start + 1], argv[arg_start + 2], argv[arg_start + 3]);
    } else if (strcmp(cmd, "reset") == 0) {
        if (args_after < 2) { fprintf(stderr, "Usage: %s reset <schema> <key>\n", argv[0]); rc = 1; }
        else rc = cmd_reset(ipc, argv[arg_start + 1], argv[arg_start + 2]);
    } else if (strcmp(cmd, "reset-all") == 0) {
        if (args_after < 1) { fprintf(stderr, "Usage: %s reset-all <schema>\n", argv[0]); rc = 1; }
        else rc = cmd_reset_all(ipc, argv[arg_start + 1]);
    } else if (strcmp(cmd, "list-keys") == 0) {
        if (args_after < 1) { fprintf(stderr, "Usage: %s list-keys <schema>\n", argv[0]); rc = 1; }
        else rc = cmd_list_keys(ipc, argv[arg_start + 1]);
    } else if (strcmp(cmd, "list-schemas") == 0) {
        rc = cmd_list_schemas(ipc);
    } else if (strcmp(cmd, "monitor") == 0) {
        if (args_after < 1) { fprintf(stderr, "Usage: %s monitor <schema> [key]\n", argv[0]); rc = 1; }
        else {
            const char *key_filter = args_after >= 2 ? argv[arg_start + 2] : NULL;
            rc = cmd_monitor(ipc, argv[arg_start + 1], key_filter);
        }
    } else {
        fprintf(stderr, "config-cli: unknown command '%s'\n", cmd);
        usage(argv[0]);
        rc = 1;
    }

    config_ipc_disconnect(ipc);
    return rc;
}
