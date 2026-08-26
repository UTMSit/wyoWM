#ifndef IPC_H
#define IPC_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
    int socket_fd;
    char *socket_path;
} IPCServer;

int ipc_init(IPCServer *server, const char *path);
void ipc_destroy(IPCServer *server);
int ipc_accept(IPCServer *server);
ssize_t ipc_read(int client_fd, void *buffer, size_t size);
ssize_t ipc_write(int client_fd, const void *buffer, size_t size);

#endif
