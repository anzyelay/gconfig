#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include "backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <errno.h>

#define MAX_LINE 4096
#define MAX_PATH 1024
#define MAX_CLIENTS FD_SETSIZE
#define MAX_NS 128

typedef struct {
    int fd;
    char rbuf[MAX_LINE * 2];
    size_t rpos, rlen;
    char **subs;
    size_t sub_cnt, sub_cap;
} client_t;

typedef struct {
    char *name;
    hashmap_t *values;
    int dirty;
    char filepath[MAX_PATH];
} ns_t;

static client_t *g_clients[MAX_CLIENTS];
static size_t g_cli_cnt;
static ns_t *g_ns[MAX_NS];
static size_t g_ns_cnt;
static char g_cfg_dir[MAX_PATH] = "./config-data";
static int g_lfd = -1;
static char g_sock_path[256];
static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig) { (void)sig; g_running = 0; }

static ns_t *ns_find(const char *name) {
    for (size_t i = 0; i < g_ns_cnt; i++)
        if (strcmp(g_ns[i]->name, name) == 0) return g_ns[i];
    return NULL;
}

struct load_ctx {
    hashmap_t *dst;
};

static void load_entry_cb(const char *key, config_value_t *val, void *ud) {
    struct load_ctx *ctx = ud;
    config_value_t *clone = value_clone(val);
    if (clone) hashmap_put(ctx->dst, key, clone);
}

static ns_t *ns_get_or_create(const char *name) {
    ns_t *ns = ns_find(name);
    if (ns) return ns;
    if (g_ns_cnt >= MAX_NS) return NULL;
    ns = calloc(1, sizeof(*ns));
    if (!ns) return NULL;
    ns->name = strdup(name);
    ns->values = hashmap_create();
    if (!ns->values || !ns->name) {
        if (ns->values) hashmap_destroy(ns->values);
        free(ns->name); free(ns); return NULL;
    }
    snprintf(ns->filepath, sizeof(ns->filepath), "%s/%s.conf", g_cfg_dir, name);
    backend_ensure_dir(g_cfg_dir);
    hashmap_t *loaded = backend_load(ns->filepath);
    if (loaded) {
        struct load_ctx ctx = { .dst = ns->values };
        hashmap_foreach(loaded, load_entry_cb, &ctx);
        hashmap_destroy(loaded);
    }
    g_ns[g_ns_cnt++] = ns;
    return ns;
}

static void send_line(client_t *c, const char *line) {
    if (!c || c->fd < 0 || !line) return;
    size_t len = strlen(line);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(c->fd, line + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += (size_t)n;
    }
    char nl = '\n';
    send(c->fd, &nl, 1, MSG_NOSIGNAL);
}

static void broadcast_notify_except(const char *ns_name, const char *key,
                                    const char *type_val, client_t *exclude) {
    char buf[MAX_LINE];
    int len;
    if (type_val && type_val[0]) {
        len = snprintf(buf, sizeof(buf), "NOTIFY %s %s %s", ns_name, key, type_val);
    } else {
        len = snprintf(buf, sizeof(buf), "NOTIFY %s %s -", ns_name, key);
    }
    if (len < 0 || (size_t)len >= sizeof(buf)) return;
    for (size_t i = 0; i < g_cli_cnt; i++) {
        client_t *c = g_clients[i];
        if (!c || c == exclude) continue;
        for (size_t j = 0; j < c->sub_cnt; j++) {
            if (strcmp(c->subs[j], ns_name) == 0) {
                send_line(c, buf);
                break;
            }
        }
    }
}

static int client_subscribe(client_t *c, const char *ns_name) {
    for (size_t i = 0; i < c->sub_cnt; i++)
        if (strcmp(c->subs[i], ns_name) == 0) return 0;
    if (c->sub_cnt >= c->sub_cap) {
        size_t ncap = c->sub_cap ? c->sub_cap * 2 : 4;
        char **tmp = realloc(c->subs, ncap * sizeof(char *));
        if (!tmp) return -1;
        c->subs = tmp;
        c->sub_cap = ncap;
    }
    c->subs[c->sub_cnt++] = strdup(ns_name);
    return 0;
}

static void client_unsubscribe(client_t *c, const char *ns_name) {
    for (size_t i = 0; i < c->sub_cnt; i++) {
        if (strcmp(c->subs[i], ns_name) == 0) {
            free(c->subs[i]);
            c->subs[i] = c->subs[--c->sub_cnt];
            return;
        }
    }
}

static void client_free(client_t *c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    for (size_t i = 0; i < c->sub_cnt; i++) free(c->subs[i]);
    free(c->subs);
    free(c);
}

struct list_ctx { char *out; size_t cap; size_t len; };

static void list_cb(const char *key, config_value_t *val, void *ud) {
    struct list_ctx *ctx = ud;
    char vbuf[256];
    if (value_to_string(val, vbuf, sizeof(vbuf)) != 0) return;
    int need = snprintf(NULL, 0, "%s:%s\n", key, vbuf) + 1;
    if (ctx->len + (size_t)need > ctx->cap) {
        size_t nc = ctx->cap ? ctx->cap * 2 : 4096;
        char *tmp = realloc(ctx->out, nc);
        if (!tmp) return;
        ctx->out = tmp;
        ctx->cap = nc;
    }
    int wrote = snprintf(ctx->out + ctx->len, ctx->cap - ctx->len, "%s:%s\n", key, vbuf);
    if (wrote > 0) ctx->len += (size_t)wrote;
}

static void handle_cmd(client_t *c, char *line) {
    if (!line || !line[0]) return;

    char *saveptr;
    char *cmd = strtok_r(line, " ", &saveptr);
    if (!cmd) return;

    if (strcmp(cmd, "PING") == 0) {
        send_line(c, "PONG");
        return;
    }

    if (strcmp(cmd, "QUIT") == 0) {
        send_line(c, "BYE");
        return;
    }

    if (strcmp(cmd, "LIST_NS") == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "OK %zu", g_ns_cnt);
        send_line(c, buf);
        for (size_t i = 0; i < g_ns_cnt; i++)
            send_line(c, g_ns[i]->name);
        return;
    }

    char *ns_name = strtok_r(NULL, " ", &saveptr);
    if (!ns_name) { send_line(c, "ERROR missing namespace"); return; }

    ns_t *ns = ns_get_or_create(ns_name);
    if (!ns) { send_line(c, "ERROR too many namespaces"); return; }

    if (strcmp(cmd, "GET") == 0) {
        char *key = strtok_r(NULL, " ", &saveptr);
        if (!key) { send_line(c, "ERROR missing key"); return; }
        config_value_t *v = hashmap_get(ns->values, key);
        if (!v) { send_line(c, "ERROR key not found"); return; }
        char vbuf[256];
        if (value_to_string(v, vbuf, sizeof(vbuf)) != 0) {
            send_line(c, "ERROR serialization failed");
            return;
        }
        char rbuf[320];
        snprintf(rbuf, sizeof(rbuf), "OK %s", vbuf);
        send_line(c, rbuf);
        return;
    }

    if (strcmp(cmd, "SET") == 0) {
        char *key = strtok_r(NULL, " ", &saveptr);
        if (!key) { send_line(c, "ERROR missing key"); return; }
        char *tval = saveptr;
        while (*tval == ' ') tval++;
        if (!*tval) { send_line(c, "ERROR missing value"); return; }
        config_value_t *parsed = calloc(1, sizeof(*parsed));
        if (!parsed) { send_line(c, "ERROR allocation failed"); return; }
        if (value_from_string(parsed, tval) != 0) {
            free(parsed);
            send_line(c, "ERROR invalid value format");
            return;
        }
        config_value_t *vp = value_clone(parsed);
        value_free(parsed);
        if (!vp) { send_line(c, "ERROR allocation failed"); return; }
        hashmap_put(ns->values, key, vp);
        ns->dirty = 1;
        send_line(c, "OK");
        broadcast_notify_except(ns_name, key, tval, c);
        return;
    }

    if (strcmp(cmd, "RESET") == 0) {
        char *key = strtok_r(NULL, " ", &saveptr);
        if (!key) { send_line(c, "ERROR missing key"); return; }
        hashmap_del(ns->values, key);
        ns->dirty = 1;
        send_line(c, "OK");
        broadcast_notify_except(ns_name, key, "", c);
        return;
    }

    if (strcmp(cmd, "RESET_ALL") == 0) {
        hashmap_destroy(ns->values);
        ns->values = hashmap_create();
        ns->dirty = 1;
        send_line(c, "OK");
        return;
    }

    if (strcmp(cmd, "LIST") == 0) {
        struct list_ctx ctx = {NULL, 0, 0};
        hashmap_foreach(ns->values, list_cb, &ctx);
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "OK %zu", hashmap_size(ns->values));
        send_line(c, hdr);
        if (ctx.out && ctx.len > 0) {
            size_t pos = 0;
            while (pos < ctx.len) {
                char *nl = strchr(ctx.out + pos, '\n');
                if (!nl) break;
                *nl = '\0';
                send_line(c, ctx.out + pos);
                pos = (size_t)(nl - ctx.out) + 1;
            }
        }
        free(ctx.out);
        return;
    }

    if (strcmp(cmd, "SYNC") == 0) {
        if (ns->dirty) {
            backend_save(ns->filepath, ns->values);
            ns->dirty = 0;
        }
        send_line(c, "OK");
        return;
    }

    if (strcmp(cmd, "SUBSCRIBE") == 0) {
        if (client_subscribe(c, ns_name) == 0)
            send_line(c, "OK");
        else
            send_line(c, "ERROR subscribe failed");
        return;
    }

    if (strcmp(cmd, "UNSUBSCRIBE") == 0) {
        client_unsubscribe(c, ns_name);
        send_line(c, "OK");
        return;
    }

    send_line(c, "ERROR unknown command");
}

static int create_listen_socket(const char *path) {
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    if (listen(fd, SOMAXCONN) < 0) {
        close(fd); return -1;
    }
    return fd;
}

int main(int argc, char *argv[]) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    const char *sock_path = "/tmp/configd.sock";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            sock_path = argv[++i];
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            snprintf(g_cfg_dir, sizeof(g_cfg_dir), "%s", argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: configd [-s socket_path] [-d config_dir]\n");
            printf("  -s  Unix socket path (default: /tmp/configd.sock)\n");
            printf("  -d  Config directory (default: ./config-data)\n");
            return 0;
        }
    }

    snprintf(g_sock_path, sizeof(g_sock_path), "%s", sock_path);
    g_lfd = create_listen_socket(g_sock_path);
    if (g_lfd < 0) {
        fprintf(stderr, "configd: cannot bind to %s: %s\n",
                g_sock_path, strerror(errno));
        return 1;
    }

    printf("configd: listening on %s, config dir: %s\n", g_sock_path, g_cfg_dir);
    fflush(stdout);

    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_lfd, &rfds);
        int maxfd = g_lfd;

        for (size_t i = 0; i < g_cli_cnt; i++) {
            if (g_clients[i] && g_clients[i]->fd >= 0) {
                FD_SET(g_clients[i]->fd, &rfds);
                if (g_clients[i]->fd > maxfd) maxfd = g_clients[i]->fd;
            }
        }

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (FD_ISSET(g_lfd, &rfds)) {
            int cfd = accept(g_lfd, NULL, NULL);
            if (cfd >= 0) {
                if (g_cli_cnt < MAX_CLIENTS) {
                    client_t *nc = calloc(1, sizeof(*nc));
                    if (nc) {
                        nc->fd = cfd;
                        g_clients[g_cli_cnt++] = nc;
                    } else {
                        close(cfd);
                    }
                } else {
                    close(cfd);
                }
            }
            n--;
        }

        for (size_t i = 0; i < g_cli_cnt && n > 0; ) {
            client_t *c = g_clients[i];
            if (!c || c->fd < 0) { i++; continue; }
            if (!FD_ISSET(c->fd, &rfds)) { i++; continue; }
            n--;

            ssize_t r = recv(c->fd, c->rbuf + c->rlen,
                             sizeof(c->rbuf) - c->rlen - 1, 0);
            if (r <= 0) {
                client_free(c);
                g_clients[i] = g_clients[--g_cli_cnt];
                continue;
            }
            c->rlen += (size_t)r;
            c->rbuf[c->rlen] = '\0';

            int removed = 0;
            while (c->rpos < c->rlen && !removed) {
                char *nl = memchr(c->rbuf + c->rpos, '\n', c->rlen - c->rpos);
                if (!nl) {
                    if (c->rpos > 0 && c->rpos < c->rlen) {
                        memmove(c->rbuf, c->rbuf + c->rpos, c->rlen - c->rpos);
                        c->rlen -= c->rpos;
                        c->rpos = 0;
                    } else if (c->rpos >= c->rlen) {
                        c->rpos = c->rlen = 0;
                    }
                    break;
                }
                *nl = '\0';
                char *cline = c->rbuf + c->rpos;
                c->rpos = (size_t)(nl + 1 - c->rbuf);

                size_t ll = strlen(cline);
                if (ll > 0 && cline[ll - 1] == '\r') cline[ll - 1] = '\0';

                handle_cmd(c, cline);

                if (strncmp(cline, "QUIT", 4) == 0 &&
                    (cline[4] == '\0' || cline[4] == ' ')) {
                    client_free(c);
                    g_clients[i] = g_clients[--g_cli_cnt];
                    removed = 1;
                }
            }
            if (!removed) i++;
        }
    }

    for (size_t i = 0; i < g_cli_cnt; i++) client_free(g_clients[i]);
    for (size_t i = 0; i < g_ns_cnt; i++) {
        if (g_ns[i]->dirty) backend_save(g_ns[i]->filepath, g_ns[i]->values);
        hashmap_destroy(g_ns[i]->values);
        free(g_ns[i]->name);
        free(g_ns[i]);
    }
    if (g_lfd >= 0) close(g_lfd);
    unlink(g_sock_path);
    return 0;
}
