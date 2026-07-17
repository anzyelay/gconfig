#include "ipc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define RBUF_SIZE 8192

struct config_ipc_t {
    int fd;
    char recv_buf[RBUF_SIZE];
    size_t recv_pos;
    size_t recv_len;
};

config_ipc_t *config_ipc_connect(const char *socket_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return NULL;
    }

    config_ipc_t *ipc = calloc(1, sizeof(*ipc));
    if (!ipc) { close(fd); return NULL; }
    ipc->fd = fd;
    return ipc;
}

void config_ipc_disconnect(config_ipc_t *ipc) {
    if (!ipc) return;
    if (ipc->fd >= 0) close(ipc->fd);
    free(ipc);
}

int config_ipc_send_fmt(config_ipc_t *ipc, const char *fmt, ...) {
    if (!ipc || ipc->fd < 0) return -1;
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len < 0 || (size_t)len >= sizeof(buf)) return -1;

    if (len == 0 || buf[len - 1] != '\n') {
        if ((size_t)len + 1 < sizeof(buf)) {
            buf[len++] = '\n';
            buf[len] = '\0';
        }
    }

    size_t sent = 0;
    while (sent < (size_t)len) {
        ssize_t n = send(ipc->fd, buf + sent, (size_t)len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

int config_ipc_recv_line(config_ipc_t *ipc, char *buf, size_t size) {
    if (!ipc || ipc->fd < 0 || !buf || size == 0) return -1;

    size_t pos = 0;
    while (pos < size - 1) {
        if (ipc->recv_pos < ipc->recv_len) {
            char c = ipc->recv_buf[ipc->recv_pos++];
            if (c == '\n') {
                buf[pos] = '\0';
                if (pos > 0 && buf[pos - 1] == '\r') buf[pos - 1] = '\0';
                return 0;
            }
            buf[pos++] = c;
        } else {
            ssize_t n = recv(ipc->fd, ipc->recv_buf, sizeof(ipc->recv_buf), 0);
            if (n <= 0) return -1;
            ipc->recv_len = (size_t)n;
            ipc->recv_pos = 0;
        }
    }
    buf[pos] = '\0';
    return 0;
}

int config_ipc_get_fd(config_ipc_t *ipc) {
    return ipc ? ipc->fd : -1;
}
