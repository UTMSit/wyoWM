#define _GNU_SOURCE
#include "ipc.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>

int ipc_init(IPCServer *server, const char *path) {
    server->socket_fd = -1;
    server->socket_path = NULL;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    unlink(path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    chmod(path, 0700);

    if (listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }

    char *copy = strdup(path);
    if (!copy) {
        close(fd);
        return -1;
    }

    server->socket_fd = fd;
    server->socket_path = copy;

    return 0;
}

void ipc_destroy(IPCServer *server) {
    if (server->socket_fd >= 0) {
        close(server->socket_fd);
        server->socket_fd = -1;
    }

    if (server->socket_path) {
        unlink(server->socket_path);
        free(server->socket_path);
        server->socket_path = NULL;
    }
}
