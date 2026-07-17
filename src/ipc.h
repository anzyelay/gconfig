#ifndef IPC_H
#define IPC_H

#include <stddef.h>

typedef struct config_ipc_t config_ipc_t;

config_ipc_t *config_ipc_connect(const char *socket_path);
void config_ipc_disconnect(config_ipc_t *ipc);
int config_ipc_send_fmt(config_ipc_t *ipc, const char *fmt, ...);
int config_ipc_recv_line(config_ipc_t *ipc, char *buf, size_t size);
int config_ipc_get_fd(config_ipc_t *ipc);

#endif
